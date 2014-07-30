/* Aseprite
 * Copyright (C) 2001-2013  David Capello
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/document.h"

#include "app/document_api.h"
#include "app/document_undo.h"
#include "app/file/format_options.h"
#include "app/flatten.h"
#include "app/objects_container_impl.h"
#include "app/undoers/add_image.h"
#include "app/undoers/add_layer.h"
#include "app/util/boundary.h"
#include "base/memory.h"
#include "base/mutex.h"
#include "base/scoped_lock.h"
#include "base/unique_ptr.h"
#include "doc/document_event.h"
#include "doc/document_observer.h"
#include "raster/cel.h"
#include "raster/layer.h"
#include "raster/mask.h"
#include "raster/palette.h"
#include "raster/sprite.h"
#include "raster/stock.h"

namespace app {

using namespace base;
using namespace raster;

Document::Document(Sprite* sprite)
  : m_undo(new DocumentUndo)
  , m_associated_to_file(false)
  , m_mutex(new mutex)
  , m_write_lock(false)
  , m_read_locks(0)
    // Information about the file format used to load/save this document
  , m_format_options(NULL)
    // Extra cel
  , m_extraCel(NULL)
  , m_extraImage(NULL)
  // Mask
  , m_mask(new Mask())
  , m_maskVisible(true)
{
  setFilename("Sprite");

  // Boundary stuff
  m_bound.nseg = 0;
  m_bound.seg = NULL;

  if (sprite)
    sprites().add(sprite);
}

Document::~Document()
{
  // We cannot be in a context at this moment. If we were in a
  // context, doc::~Document() would remove the document from the
  // context and it would generate onRemoveDocument() notifications,
  // which could result in serious problems for observers expecting a
  // fully created app::Document.
  ASSERT(context() == NULL);

  if (m_bound.seg)
    base_free(m_bound.seg);

  destroyExtraCel();
}

DocumentApi Document::getApi(undo::UndoersCollector* undoers)
{
  return DocumentApi(this, undoers ? undoers: m_undo->getDefaultUndoersCollector());
}

void Document::notifyGeneralUpdate()
{
  doc::DocumentEvent ev(this);
  notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onGeneralUpdate, ev);
}

void Document::notifySpritePixelsModified(Sprite* sprite, const gfx::Region& region)
{
  doc::DocumentEvent ev(this);
  ev.sprite(sprite);
  ev.region(region);
  notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onSpritePixelsModified, ev);
}

void Document::notifyLayerMergedDown(Layer* srcLayer, Layer* targetLayer)
{
  doc::DocumentEvent ev(this);
  ev.sprite(srcLayer->sprite());
  ev.layer(srcLayer);
  ev.targetLayer(targetLayer);
  notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onLayerMergedDown, ev);
}

void Document::notifyCelMoved(Layer* fromLayer, FrameNumber fromFrame, Layer* toLayer, FrameNumber toFrame)
{
  doc::DocumentEvent ev(this);
  ev.sprite(fromLayer->sprite());
  ev.layer(fromLayer);
  ev.frame(fromFrame);
  ev.targetLayer(toLayer);
  ev.targetFrame(toFrame);
  notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onCelMoved, ev);
}

void Document::notifyCelCopied(Layer* fromLayer, FrameNumber fromFrame, Layer* toLayer, FrameNumber toFrame)
{
  doc::DocumentEvent ev(this);
  ev.sprite(fromLayer->sprite());
  ev.layer(fromLayer);
  ev.frame(fromFrame);
  ev.targetLayer(toLayer);
  ev.targetFrame(toFrame);
  notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onCelCopied, ev);
}

bool Document::isModified() const
{
  return !m_undo->isSavedState();
}

bool Document::isAssociatedToFile() const
{
  return m_associated_to_file;
}

void Document::markAsSaved()
{
  m_undo->markSavedState();
  m_associated_to_file = true;
}

void Document::impossibleToBackToSavedState()
{
  m_undo->impossibleToBackToSavedState();
}

//////////////////////////////////////////////////////////////////////
// Loaded options from file

void Document::setFormatOptions(const SharedPtr<FormatOptions>& format_options)
{
  m_format_options = format_options;
}

//////////////////////////////////////////////////////////////////////
// Boundaries

int Document::getBoundariesSegmentsCount() const
{
  return m_bound.nseg;
}

const BoundSeg* Document::getBoundariesSegments() const
{
  return m_bound.seg;
}

void Document::generateMaskBoundaries(Mask* mask)
{
  if (m_bound.seg) {
    base_free(m_bound.seg);
    m_bound.seg = NULL;
    m_bound.nseg = 0;
  }

  // No mask specified? Use the current one in the document
  if (!mask) {
    if (!isMaskVisible())       // The mask is hidden
      return;                   // Done, without boundaries
    else
      mask = this->mask();      // Use the document mask
  }

  ASSERT(mask != NULL);

  if (!mask->isEmpty()) {
    m_bound.seg = find_mask_boundary(mask->bitmap(),
                                     &m_bound.nseg,
                                     IgnoreBounds, 0, 0, 0, 0);
    for (int c=0; c<m_bound.nseg; c++) {
      m_bound.seg[c].x1 += mask->bounds().x;
      m_bound.seg[c].y1 += mask->bounds().y;
      m_bound.seg[c].x2 += mask->bounds().x;
      m_bound.seg[c].y2 += mask->bounds().y;
    }
  }
}

//////////////////////////////////////////////////////////////////////
// Extra Cel (it is used to draw pen preview, pixels in movement, etc.)

void Document::destroyExtraCel()
{
  delete m_extraCel;
  delete m_extraImage;

  m_extraCel = NULL;
  m_extraImage = NULL;
}

void Document::prepareExtraCel(int x, int y, int w, int h, int opacity)
{
  ASSERT(sprite() != NULL);

  if (!m_extraCel)
    m_extraCel = new Cel(FrameNumber(0), 0); // Ignored fields for this cell (frame, and image index)

  m_extraCel->setPosition(x, y);
  m_extraCel->setOpacity(opacity);

  if (!m_extraImage ||
      m_extraImage->pixelFormat() != sprite()->pixelFormat() ||
      m_extraImage->width() != w ||
      m_extraImage->height() != h) {
    delete m_extraImage;                // image
    m_extraImage = Image::create(sprite()->pixelFormat(), w, h);
  }
}

Cel* Document::getExtraCel() const
{
  return m_extraCel;
}

Image* Document::getExtraCelImage() const
{
  return m_extraImage;
}

//////////////////////////////////////////////////////////////////////
// Mask

void Document::setMask(const Mask* mask)
{
  m_mask.reset(new Mask(*mask));
  m_maskVisible = true;

  resetTransformation();
}

bool Document::isMaskVisible() const
{
  return
    m_maskVisible &&            // The mask was not hidden by the user explicitly
    m_mask &&                   // The mask does exist
    !m_mask->isEmpty();         // The mask is not empty
}

void Document::setMaskVisible(bool visible)
{
  m_maskVisible = visible;
}

//////////////////////////////////////////////////////////////////////
// Transformation

gfx::Transformation Document::getTransformation() const
{
  return m_transformation;
}

void Document::setTransformation(const gfx::Transformation& transform)
{
  m_transformation = transform;
}

void Document::resetTransformation()
{
  if (m_mask)
    m_transformation = gfx::Transformation(m_mask->bounds());
  else
    m_transformation = gfx::Transformation();
}

//////////////////////////////////////////////////////////////////////
// Copying

void Document::copyLayerContent(const Layer* sourceLayer0, Document* destDoc, Layer* destLayer0) const
{
  // Copy the layer name
  destLayer0->setName(sourceLayer0->name());

  if (sourceLayer0->isImage() && destLayer0->isImage()) {
    const LayerImage* sourceLayer = static_cast<const LayerImage*>(sourceLayer0);
    LayerImage* destLayer = static_cast<LayerImage*>(destLayer0);

    // copy cels
    CelConstIterator it = sourceLayer->getCelBegin();
    CelConstIterator end = sourceLayer->getCelEnd();

    for (; it != end; ++it) {
      const Cel* sourceCel = *it;
      base::UniquePtr<Cel> newCel(new Cel(*sourceCel));

      const Image* sourceImage = sourceCel->image();
      ASSERT(sourceImage != NULL);

      Image* newImage = Image::createCopy(sourceImage);
      newCel->setImage(destLayer->sprite()->stock()->addImage(newImage));

      destLayer->addCel(newCel);
      newCel.release();
    }
  }
  else if (sourceLayer0->isFolder() && destLayer0->isFolder()) {
    const LayerFolder* sourceLayer = static_cast<const LayerFolder*>(sourceLayer0);
    LayerFolder* destLayer = static_cast<LayerFolder*>(destLayer0);

    LayerConstIterator it = sourceLayer->getLayerBegin();
    LayerConstIterator end = sourceLayer->getLayerEnd();

    for (; it != end; ++it) {
      Layer* sourceChild = *it;
      base::UniquePtr<Layer> destChild(NULL);

      if (sourceChild->isImage()) {
        destChild.reset(new LayerImage(destLayer->sprite()));
        copyLayerContent(sourceChild, destDoc, destChild);
      }
      else if (sourceChild->isFolder()) {
        destChild.reset(new LayerFolder(destLayer->sprite()));
        copyLayerContent(sourceChild, destDoc, destChild);
      }
      else {
        ASSERT(false);
      }

      ASSERT(destChild != NULL);

      // Add the new layer in the sprite.

      Layer* newLayer = destChild.release();
      Layer* afterThis = destLayer->getLastLayer();

      destLayer->addLayer(newLayer);
      destChild.release();

      destLayer->stackLayer(newLayer, afterThis);
    }
  }
  else  {
    ASSERT(false && "Trying to copy two incompatible layers");
  }
}

Document* Document::duplicate(DuplicateType type) const
{
  const Sprite* sourceSprite = sprite();
  base::UniquePtr<Sprite> spriteCopyPtr(new Sprite(
      sourceSprite->pixelFormat(),
      sourceSprite->width(),
      sourceSprite->height(),
      sourceSprite->getPalette(FrameNumber(0))->size()));

  base::UniquePtr<Document> documentCopy(new Document(spriteCopyPtr));
  Sprite* spriteCopy = spriteCopyPtr.release();

  spriteCopy->setTotalFrames(sourceSprite->totalFrames());

  // Copy frames duration
  for (FrameNumber i(0); i < sourceSprite->totalFrames(); ++i)
    spriteCopy->setFrameDuration(i, sourceSprite->getFrameDuration(i));

  // Copy color palettes
  {
    PalettesList::const_iterator it = sourceSprite->getPalettes().begin();
    PalettesList::const_iterator end = sourceSprite->getPalettes().end();
    for (; it != end; ++it) {
      const Palette* pal = *it;
      spriteCopy->setPalette(pal, true);
    }
  }

  switch (type) {

    case DuplicateExactCopy:
      // Copy the layer folder
      copyLayerContent(sprite()->folder(), documentCopy, spriteCopy->folder());
      break;

    case DuplicateWithFlattenLayers:
      {
        // Flatten layers
        ASSERT(sourceSprite->folder() != NULL);

        LayerImage* flatLayer = create_flatten_layer_copy
            (spriteCopy,
             sourceSprite->folder(),
             gfx::Rect(0, 0, sourceSprite->width(), sourceSprite->height()),
             FrameNumber(0), sourceSprite->lastFrame());

        // Add and select the new flat layer
        spriteCopy->folder()->addLayer(flatLayer);

        // Configure the layer as background only if the original
        // sprite has a background layer.
        if (sourceSprite->backgroundLayer() != NULL)
          flatLayer->configureAsBackground();
      }
      break;
  }

  documentCopy->setMask(mask());
  documentCopy->m_maskVisible = m_maskVisible;
  documentCopy->generateMaskBoundaries();

  return documentCopy.release();
}

//////////////////////////////////////////////////////////////////////
// Multi-threading ("sprite wrappers" use this)

bool Document::lock(LockType lockType)
{
  scoped_lock lock(*m_mutex);

  switch (lockType) {

    case ReadLock:
      // If no body is writting the sprite...
      if (!m_write_lock) {
        // We can read it
        ++m_read_locks;
        return true;
      }
      break;

    case WriteLock:
      // If no body is reading and writting...
      if (m_read_locks == 0 && !m_write_lock) {
        // We can start writting the sprite...
        m_write_lock = true;
        return true;
      }
      break;

  }

  return false;
}

bool Document::lockToWrite()
{
  scoped_lock lock(*m_mutex);

  // this only is possible if there are just one reader
  if (m_read_locks == 1) {
    ASSERT(!m_write_lock);
    m_read_locks = 0;
    m_write_lock = true;
    return true;
  }
  else
    return false;
}

void Document::unlockToRead()
{
  scoped_lock lock(*m_mutex);

  ASSERT(m_read_locks == 0);
  ASSERT(m_write_lock);

  m_write_lock = false;
  m_read_locks = 1;
}

void Document::unlock()
{
  scoped_lock lock(*m_mutex);

  if (m_write_lock) {
    m_write_lock = false;
  }
  else if (m_read_locks > 0) {
    --m_read_locks;
  }
  else {
    ASSERT(false);
  }
}

} // namespace app

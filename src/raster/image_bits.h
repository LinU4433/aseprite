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

#ifndef RASTER_IMAGE_BITS_H_INCLUDED
#define RASTER_IMAGE_BITS_H_INCLUDED
#pragma once

#include "raster/image.h"
#include "raster/image_iterator.h"

namespace raster {

  template<typename ImageTraits>
  class ImageBits {
  public:
    typedef typename ImageTraits::address_t address_t;
    typedef ImageIterator<ImageTraits> iterator;
    typedef ImageConstIterator<ImageTraits> const_iterator;

    ImageBits() :
      m_image(NULL),
      m_bounds(0, 0, 0, 0) {
    }

    ImageBits(const ImageBits& other) :
      m_image(other.m_image),
      m_bounds(other.m_bounds) {
    }

    ImageBits(Image* image, const gfx::Rect& bounds) :
      m_image(image),
      m_bounds(bounds) {
      ASSERT(bounds.x >= 0 && bounds.x+bounds.w <= image->width() &&
             bounds.y >= 0 && bounds.y+bounds.h <= image->height());
    }

    ImageBits& operator=(const ImageBits& other) {
      m_image = other.m_image;
      m_bounds = other.m_bounds;
      return *this;
    }

    // Iterate over the full area.
    iterator begin() {
      return iterator(m_image, m_bounds, m_bounds.x, m_bounds.y);
    }
    iterator end() {
      iterator it(m_image, m_bounds, m_bounds.x+m_bounds.w-1, m_bounds.y+m_bounds.h-1);
      ++it;
      return it;
    }

    const_iterator begin() const {
      return const_iterator(m_image, m_bounds, m_bounds.x, m_bounds.y);
    }
    const_iterator end() const {
      const_iterator it(m_image, m_bounds, m_bounds.x+m_bounds.w-1, m_bounds.y+m_bounds.h-1);
      ++it;
      return it;
    }

    // Iterate over a sub-area.
    iterator begin_area(const gfx::Rect& area) {
      ASSERT(m_bounds.contains(area));
      return iterator(m_image, area, area.x, area.y);
    }
    iterator end_area(const gfx::Rect& area) {
      ASSERT(m_bounds.contains(area));
      iterator it(m_image, area, area.x+area.w-1, area.y+area.h-1);
      ++it;
      return it;
    }

    const_iterator begin_area(const gfx::Rect& area) const {
      ASSERT(m_bounds.contains(area));
      return const_iterator(m_image, area, area.x, area.y);
    }
    const_iterator end_area(const gfx::Rect& area) const {
      ASSERT(m_bounds.contains(area));
      const_iterator it(m_image, area, area.x+area.w-1, area.y+area.h-1);
      ++it;
      return it;
    }

    Image* image() const { return m_image; }
    const gfx::Rect& bounds() { return m_bounds; }

    Image* image() { return m_image; }

    void unlock() {
      if (m_image) {
        m_image->unlockBits(*this);
        m_image = NULL;
      }
    }

  private:
    Image* m_image;
    gfx::Rect m_bounds;
  };

  template<typename ImageTraits>
  class LockImageBits {
  public:
    typedef ImageBits<ImageTraits> Bits;
    typedef typename Bits::iterator iterator;
    typedef typename Bits::const_iterator const_iterator;

    explicit LockImageBits(const Image* image)
      : m_bits(image->lockBits<ImageTraits>(Image::ReadLock, image->bounds())) {
    }

    LockImageBits(const Image* image, const gfx::Rect& bounds)
      : m_bits(image->lockBits<ImageTraits>(Image::ReadLock, bounds)) {
    }

    LockImageBits(Image* image, Image::LockType lockType)
      : m_bits(image->lockBits<ImageTraits>(lockType, image->bounds())) {
    }

    LockImageBits(Image* image, Image::LockType lockType, const gfx::Rect& bounds)
      : m_bits(image->lockBits<ImageTraits>(lockType, bounds)) {
    }

    ~LockImageBits() {
      m_bits.image()->unlockBits(m_bits);
    }

    // Iterators.
    iterator begin() { return m_bits.begin(); }
    iterator end() { return m_bits.end(); }
    const_iterator begin() const { return m_bits.begin(); }
    const_iterator end() const { return m_bits.end(); }

    iterator begin_area(const gfx::Rect& area) { return m_bits.begin_area(area); }
    iterator end_area(const gfx::Rect& area) { return m_bits.end_area(area); }
    const_iterator begin_area(const gfx::Rect& area) const { return m_bits.begin_area(area); }
    const_iterator end_area(const gfx::Rect& area) const { return m_bits.end_area(area); }

    const Image* image() const { return m_bits.image(); }
    const gfx::Rect& bounds() const { return m_bits.bounds(); }

    Image* image() { return m_bits.image(); }
    
  private:
    Bits m_bits;

    LockImageBits();            // Undefined
  };

} // namespace raster

#endif

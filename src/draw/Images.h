#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "draw/Painter.h"

// Pictures in Office documents (plan §4.5 and §4.7). Pictures are untrusted input:
//  - PNG, JPEG, GIF, BMP, TIFF and ICO are decoded by WIC, with decoders created from Windows'
//    built-in codec classes only, so a third-party codec installed on the PC never sees them;
//  - EMF is drawn by Direct2D's GDI metafile support, and WMF is converted to EMF by GDI first;
//  - SVG is drawn by Direct2D's SVG support, or replaced by the PNG copy Office stores next to it
//    when the SVG uses something Direct2D can't draw (text, for one) or that would fetch anything;
//  - sizes are capped (ImageLimits), and nothing is decoded until it is drawn.
namespace draw {

enum class ImageFormat : uint8_t { Unknown, Png, Jpeg, Gif, Bmp, Tiff, Ico, Emf, Wmf, Svg };

// The format the bytes are in, from their first bytes (never from a name or content type).
ImageFormat SniffImageFormat(const uint8_t* data, size_t size);

struct ImageLimits {
  uint64_t maxBytes = 256ull << 20;        // encoded size
  uint32_t maxDimension = 65535;           // pixels on either side, as the picture declares them
  uint64_t maxSourcePixels = 1ull << 29;   // declared width x height
  // Pixels kept in memory for one picture (16.7 megapixels, 64 MB). A larger picture is scaled
  // down while it is decoded.
  uint64_t maxDecodedPixels = 1ull << 24;

  static const ImageLimits& Default();
};

// A picture from a document. Open reads only its header; pixels are decoded when they are first
// asked for, at the size asked for, and kept. Safe to share between threads.
class Image {
 public:
  // Null, with a plain-language reason in `error`, when the bytes aren't a picture Featherweight
  // reads or break the limits.
  static std::shared_ptr<Image> Open(std::vector<uint8_t> bytes, const ImageLimits& limits = ImageLimits::Default(),
                                     std::string* error = nullptr);
  ImageFormat Format() const { return format_; }
  // Pixels, for raster pictures; 0 for vector pictures.
  int Width() const { return width_; }
  int Height() const { return height_; }
  // Points, from the resolution the picture records (96 dpi when it records none).
  Size NaturalSize() const { return natural_; }
  // The metafile or SVG to draw; null for raster pictures.
  const std::shared_ptr<const VectorImage>& Vector() const { return vector_; }
  // False for an SVG that uses something Direct2D doesn't draw (text) or that refers to anything
  // outside itself: Office's PNG copy should be shown instead.
  bool Faithful() const { return faithful_; }
  // Pixels covering at least `width` x `height` where the picture has that many (0 x 0: all of
  // them), within the limits. Decoded on first use and kept; asking for more decodes again. Vector
  // pictures are rasterized. Null when the picture can't be decoded after all.
  std::shared_ptr<const Bitmap> Pixels(int width = 0, int height = 0) const;

 private:
  Image() = default;
  std::shared_ptr<const Bitmap> Decode(int width, int height) const;

  ImageFormat format_ = ImageFormat::Unknown;
  std::vector<uint8_t> bytes_;
  ImageLimits limits_;
  int width_ = 0, height_ = 0;
  Size natural_;
  std::shared_ptr<const VectorImage> vector_;
  bool faithful_ = true;
  mutable std::mutex mutex_;
  mutable std::shared_ptr<const Bitmap> pixels_;
  mutable bool failed_ = false;
};

// Draws a vector picture into a `width` x `height` bitmap with Direct2D's software renderer: what
// Painter::DrawVectorImage does by default, and how pictures are recolored.
std::shared_ptr<const Bitmap> RasterizeVectorImage(const VectorImage& image, int width, int height);

}  // namespace draw

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "draw/Drawing.h"
#include "slide/render/SlideRender.h"

namespace fw::pkg {
class Package;
class XmlDom;
}  // namespace fw::pkg

// A deck's slides read straight from its package, as SlideSources: for fwtool and the tests, and
// until the presentation model's own slides are wired in. Read-only; parts are parsed on first use
// and kept. The package must outlive the deck.
namespace fw::slide::render {

class PackageDeck {
 public:
  explicit PackageDeck(const fw::pkg::Package& package);
  ~PackageDeck();
  PackageDeck(const PackageDeck&) = delete;
  PackageDeck& operator=(const PackageDeck&) = delete;

  size_t SlideCount() const { return slides_.size(); }
  draw::Size SlideSize() const { return size_; }
  // The slide's parts (valid while the deck lives); an empty source for a slide that can't be read.
  SlideSource Source(size_t index);
  // Whether the slide is hidden from the show (p:sld show="0").
  bool Hidden(size_t index);
  const std::string& SlidePart(size_t index) const { return slides_[index]; }

 private:
  struct Part;
  Part* Get(const std::string& name);
  std::string Related(const std::string& part, std::string_view kind) const;

  const fw::pkg::Package& package_;
  std::string presentation_;
  std::vector<std::string> slides_;
  draw::Size size_{720, 540};
  int firstNumber_ = 1;
  std::shared_ptr<draw::ImageCache> images_;
  std::map<std::string, std::unique_ptr<Part>> parts_;
  std::map<std::string, std::shared_ptr<const draw::Theme>> themes_;
};

}  // namespace fw::slide::render

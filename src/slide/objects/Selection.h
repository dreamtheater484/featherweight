#pragma once

#include <cstdint>
#include <vector>

#include "draw/Painter.h"
#include "slide/Presentation.h"
#include "slide/objects/Frame.h"
#include "slide/objects/ObjectXml.h"

// A slide's objects as selection sees them (P2, p2-objects; plan §8.2): every shape on the slide
// and in its groups, with its frame on the slide, its parent group and how the parent's
// coordinates map to the slide's. Clicking picks the topmost object at the level being edited
// (the slide, or the group entered with a second click), Tab goes through them in order, and a
// marquee picks those wholly inside it, as in PowerPoint. The master's and layout's shapes aren't
// the slide's and can't be picked.
namespace fw::slide::objects {

struct ObjectInfo {
  uint32_t id = 0;
  uint32_t parent = 0;           // the group it's in; 0 on the slide
  const fw::slide::Shape* shape = nullptr;
  ObjectKind kind = ObjectKind::Other;
  bool placeholder = false;
  bool line = false;             // a connector or a line: two ends, no turn handle
  bool lockAspect = false;       // pictures and groups keep their proportions at the corners
  Frame local;                   // in the parent's coordinates
  Frame frame;                   // on the slide
  draw::Matrix parentToSlide;    // the parent's coordinates (points) to the slide's
};

class ObjectMap {
 public:
  // The slide's own shapes (hidden ones left out), bottom to top, groups before their children.
  void Build(const fw::slide::SlideBase& slide);
  void Clear() { objects_.clear(); }

  const std::vector<ObjectInfo>& Objects() const { return objects_; }
  const ObjectInfo* Find(uint32_t id) const;
  // The objects directly in `parent` (0: on the slide), bottom to top.
  std::vector<const ObjectInfo*> Children(uint32_t parent) const;
  // Whether `id` is `ancestor` or inside it.
  bool Within(uint32_t id, uint32_t ancestor) const;
  // The object at `level` (0: the slide, else a group) that holds `id`: itself, or the group at
  // that level it's inside. 0 when `id` isn't inside `level`.
  uint32_t AtLevel(uint32_t id, uint32_t level) const;

  // The topmost object under `p` at `level` (a group is hit through its children); 0 when none.
  uint32_t HitTest(draw::Point p, uint32_t level, float tolerance) const;
  // The topmost object of any depth under `p` (for entering a group); 0 when none.
  uint32_t HitDeepest(draw::Point p, float tolerance) const;
  // The objects at `level` whose bounds lie wholly inside `marquee`, bottom to top.
  std::vector<uint32_t> InMarquee(const draw::Rect& marquee, uint32_t level) const;
  // The next object at `level` after `current` in z-order (Tab), or before it (Shift+Tab),
  // wrapping around; the first (or last) when `current` isn't there. 0 when there's none.
  uint32_t Next(uint32_t current, uint32_t level, bool back) const;

 private:
  void Add(const fw::slide::Shape& s, uint32_t parent, const draw::Matrix& parentToSlide, int depth);
  std::vector<ObjectInfo> objects_;
};

// The box around the frames of `ids` on the slide (their turned bounds).
draw::Rect SelectionBounds(const ObjectMap& map, const std::vector<uint32_t>& ids);

}  // namespace fw::slide::objects

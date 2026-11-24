#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "slide/edit/EditInternal.h"

// DrawingML writing the shape and text operations share. Internal to src/slide/edit.
namespace fw::slide::edit {

// A fill element for a color (a:solidFill, a:noFill), declaring its prefix; empty for Inherit.
std::string ColorFillXml(const ColorSpec& c);
// Replaces `parent`'s children of group `group` (groupOf maps a local name to its schema group, -1
// for none) with the element in `xml` (empty: removes them), put in schema order. False when `xml`
// isn't one element of that group.
bool ReplaceGroup(pugi::xml_node parent, std::string_view xml, const std::function<int(std::string_view)>& groupOf,
                  int group);
// spPr's child groups in schema order (xfrm 0, geometry 1, fill 2, ln 3, effects 4, scene3d 5, sp3d 6).
int SpPrGroupOf(std::string_view local);
// The effects, builds and connections in slide `root` that name shapes `ids`, which are gone (as
// deleting them does; a layout change dropping empty placeholders too).
void DropShapeRefs(Editor::Impl& m, pugi::xml_node root, const std::set<uint32_t>& ids);
// The action attribute for a link target ("ppaction://hlinksldjump"...); empty for a web address.
std::string LinkAction(const LinkTarget& link);
// Puts a shape element on slide part `part` at z position `z` (-1: on top), renewing its ids that
// collide with the slide's; `id` gets its cNvPr id. Inside a Call.
Result PutShape(Editor::Impl& m, std::string_view part, pugi::xml_node shape, int z, uint32_t* id,
                uint32_t parentGroup = 0);
// Fills an empty a:hlinkClick for `target`, adding the relationship of `part` it needs.
bool WriteLink(Editor::Impl& m, std::string_view part, pugi::xml_node link, const LinkTarget& target);

}  // namespace fw::slide::edit

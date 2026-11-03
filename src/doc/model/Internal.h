#pragma once

// Shared by the model's own sources: the caches behind Document's resolved properties.
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "doc/model/Document.h"

namespace fw::doc {

struct Document::Caches {
  // Resolved properties: indices into the pools, kNone until resolved. The pools are deques, so
  // the references Document hands out stay valid as they grow.
  std::vector<uint32_t> para;            // by paragraph
  std::deque<ParaProps> paraPool;
  std::vector<uint32_t> mark;            // by paragraph
  std::unordered_map<ElemId, uint32_t> run;  // by w:r
  std::deque<RunProps> runPool;
  // Field state at the start of each paragraph that begins inside a field: for each open field,
  // outermost first, whether it is still in its code (1) or in its result (0).
  std::vector<bool> fieldsDone;          // by story
  std::unordered_map<uint32_t, std::vector<uint8_t>> fieldStack;  // by paragraph
  // List labels, computed a story at a time.
  std::vector<bool> labelsDone;          // by story
  std::unordered_map<uint32_t, ListLabel> labels;  // by paragraph
  // Lookups.
  std::unordered_map<std::string_view, uint32_t> styleIndex;  // style id -> index into Styles()
  std::unordered_map<std::string_view, ElemId> nums, abstractNums;  // w:numId / w:abstractNumId
  std::unordered_map<std::string_view, uint32_t> commentStory;  // comment id -> story
  std::unordered_map<std::string_view, uint32_t> footnoteStory, endnoteStory;
  std::deque<std::string> owned;  // strings made by resolution (theme font names); a deque keeps them in place

  std::string_view Own(std::string s) {
    owned.push_back(std::move(s));
    return owned.back();
  }
};

// A list label's formatting: the paragraph mark's, then the level's w:rPr (Resolve.cpp).
RunProps LabelProps(const Document& doc, Document::Caches& caches, uint32_t para, Node lvl);

// The element names the model's code uses, as hashes.
namespace names {
inline constexpr uint32_t kP = Name("p"), kR = Name("r"), kT = Name("t"), kTbl = Name("tbl"), kTr = Name("tr"), kTc = Name("tc");
inline constexpr uint32_t kSdt = Name("sdt"), kSdtContent = Name("sdtContent"), kCustomXml = Name("customXml");
inline constexpr uint32_t kPPr = Name("pPr"), kRPr = Name("rPr"), kSectPr = Name("sectPr"), kTblPr = Name("tblPr");
inline constexpr uint32_t kTrPr = Name("trPr"), kTcPr = Name("tcPr"), kTblGrid = Name("tblGrid"), kGridCol = Name("gridCol");
inline constexpr uint32_t kVal = Name("val"), kId = Name("id"), kType = Name("type"), kW = Name("w");
}  // namespace names

}  // namespace fw::doc

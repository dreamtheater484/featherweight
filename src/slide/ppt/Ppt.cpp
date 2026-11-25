// PowerPoint 97-2003 presentations ([MS-PPT]) as .pptx packages: the persist directory's newest
// objects (CurrentUserAtom, the UserEditAtom chain), the document (slide size, fonts, the default
// text styles, the drawing group's pictures and default shape properties, hyperlinks), its masters
// (main masters as slide masters, title masters as their layouts), slides and notes. Shapes and text
// are written by Shapes.cpp and Text.cpp.
#include "slide/ppt/Ppt.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <pugixml.hpp>

#include "core/Lang.h"
#include "draw/OfficeArt.h"
#include "pkg/OfficeCrypto.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "slide/ppt/Internal.h"

namespace fw::slide::ppt {
namespace {

using namespace detail;

constexpr const char* kDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
constexpr const char* kNs =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
constexpr const char* kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr const char* kTree =
    "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
    "<a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>";
constexpr const char* kClrMap =
    "bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
    "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"";

std::string Num(long long v) { return std::to_string(v); }

// A part's relationships.
class Rels {
 public:
  std::string Add(std::string_view type, std::string_view target, bool external = false) {
    const std::string key = std::string(type) + "|" + std::string(target);
    if (auto it = known_.find(key); it != known_.end()) return it->second;
    const std::string id = "rId" + Num(next_++);
    xml_ += "<Relationship Id=\"" + id + "\" Type=\"" + kRel + std::string(type) + "\" Target=\"" + Esc(target) + "\"" +
            (external ? " TargetMode=\"External\"" : "") + "/>";
    known_.emplace(key, id);
    return id;
  }
  std::string Xml() const {
    return std::string(kDecl) + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" + xml_ +
           "</Relationships>";
  }

 private:
  std::string xml_;
  int next_ = 1;
  std::map<std::string, std::string> known_;
};

// One entry of a SlideListWithText: the persist object, its id and the outline's texts after it.
struct ListEntry {
  uint32_t persist = 0, id = 0, flags = 0;
  std::vector<Text> texts;
};

class Converter {
 public:
  std::wstring password;
  bool stopAtEncryption = false, encrypted = false;

  std::vector<uint8_t> Convert(const StreamFn& streams, pkg::Status& status) {
    auto docStream = streams("PowerPoint Document");
    if (!docStream) {
      status = pkg::Status::Fail(pkg::Status::Code::Damaged, T("it isn't a PowerPoint presentation or it is damaged."));
      return {};
    }
    doc_ = std::move(*docStream);
    if (auto pictures = streams("Pictures")) pictures_ = std::move(*pictures);
    if (!ReadPersist(streams, status)) return {};
    if (stopAtEncryption) return {};
    if (encrypted) {
      if (!Decrypt(status)) return {};
      if (!Persist(docPersist_) || Persist(docPersist_)->type != 0x03E8) {
        status = pkg::Status::Fail(pkg::Status::Code::Damaged, T("it isn't a PowerPoint presentation or it is damaged."));
        return {};
      }
    }
    if (!ReadDocument(status)) return {};
    return Write(status);
  }

 private:
  // ---- The persist directory: each object's newest place in the stream.

  std::optional<Rec> RecAt(uint32_t offset) const {
    if (offset >= doc_.size()) return std::nullopt;
    Records rs(std::span<const uint8_t>(doc_).subspan(offset));
    Rec r;
    if (!rs.Next(r)) return std::nullopt;
    return r;
  }
  std::optional<Rec> Persist(uint32_t id) const {
    const auto it = persist_.find(id);
    if (it == persist_.end()) return std::nullopt;
    return RecAt(it->second);
  }

  bool ReadPersist(const StreamFn& streams, pkg::Status& status) {
    uint32_t offset = UINT32_MAX;
    if (auto user = streams("Current User")) {
      Records rs(*user);
      Rec r;
      if (rs.Next(r) && r.type == 0x0FF6) {
        Bytes b(r.body);
        b.U32();  // size
        const uint32_t token = b.U32();
        offset = b.U32();
        if (!b.Ok()) offset = UINT32_MAX;
        if (token == 0xF3D1C4DF) encrypted = true;
      }
    }
    // Without a usable CurrentUserAtom, the last UserEditAtom at the stream's top level.
    if (!RecAt(offset) || RecAt(offset)->type != 0x0FF5) {
      offset = UINT32_MAX;
      Records rs(doc_);
      size_t at = 0;
      for (Rec r; rs.Next(r);) {
        if (r.type == 0x0FF5) offset = static_cast<uint32_t>(at);
        at += 8 + r.body.size();
      }
    }
    std::set<uint32_t> seen;
    bool first = true;
    while (offset != UINT32_MAX && seen.insert(offset).second && seen.size() < 4096) {
      const auto edit = RecAt(offset);
      if (!edit || edit->type != 0x0FF5) break;
      Bytes b(edit->body);
      b.U32();  // lastSlideIdRef
      b.U16();
      b.U8();
      b.U8();
      const uint32_t last = b.U32(), directory = b.U32(), docRef = b.U32();
      b.U32();  // persistIdSeed
      b.U16();
      b.U16();
      if (!b.Ok()) break;
      if (first) {
        docPersist_ = docRef;
        if (edit->body.size() >= 32) {
          encryptPersist_ = b.U32();
          encrypted = encrypted || encryptPersist_ != 0;
        }
        first = false;
      }
      if (const auto dir = RecAt(directory); dir && dir->type == 0x1772) {
        Bytes d(dir->body);
        while (d.Left() >= 4) {
          const uint32_t head = d.U32();
          const uint32_t id = head & 0xFFFFF, count = head >> 20;
          for (uint32_t k = 0; k < count && d.Ok(); ++k) {
            const uint32_t at = d.U32();
            if (d.Ok()) persist_.emplace(id + k, at);  // the newest edit's place wins
          }
          if (persist_.size() > 1000000) break;
        }
      }
      offset = last ? last : UINT32_MAX;
    }
    if (docPersist_ == 0 && !encrypted) {
      status = pkg::Status::Fail(pkg::Status::Code::Damaged, T("it isn't a PowerPoint presentation or it is damaged."));
      return false;
    }
    if (!encrypted && (!Persist(docPersist_) || Persist(docPersist_)->type != 0x03E8)) {
      status = pkg::Status::Fail(pkg::Status::Code::Damaged, T("it isn't a PowerPoint presentation or it is damaged."));
      return false;
    }
    return true;
  }

  // ---- Encryption ([MS-PPT] 2.3.7): RC4 CryptoAPI. Each persist object but the
  // CryptSession10Container is encrypted, its header with it, with its persist id as the block
  // number; each record of the Pictures stream with block 0. The UserEditAtoms and the persist
  // directories stay clear.

  bool Decrypt(pkg::Status& status) {
    using pkg::crypto::Result;
    const auto session = encryptPersist_ ? Persist(encryptPersist_) : std::nullopt;
    pkg::crypto::Rc4Key key;
    const Result r = session && session->type == 0x2F14 ? key.Open(session->body, password) : Result::Damaged;
    if (r == Result::WrongPassword) {
      status = pkg::Status::Fail(pkg::Status::Code::Rejected,
                                 password.empty() ? T("it is protected with a password.") : T("the password is wrong."));
      return false;
    }
    if (r != Result::Ok) {
      status = pkg::Status::Fail(r == Result::Damaged ? pkg::Status::Code::Damaged : pkg::Status::Code::Unsupported,
                                 r == Result::Damaged ? T("it seems to be damaged.") : T("it is protected in a way Featherweight doesn't support."));
      return false;
    }
    std::set<uint32_t> done;  // a damaged directory may name a place twice
    for (const auto& [id, at] : persist_)
      if (id != encryptPersist_ && done.insert(at).second) DecryptRecord(key, id, doc_, at);
    for (size_t at = 0; at < pictures_.size();) at += DecryptPicture(key, pictures_, at);
    return true;
  }

  // A Pictures stream record (an OfficeArt BLIP), in place: each piece restarts block 0's key
  // stream: the header, each 16-byte id, the metafile header (EMF, WMF, PICT) or the tag byte, then
  // the picture. Returns the record's size, header included.
  static size_t DecryptPicture(const pkg::crypto::Rc4Key& key, std::vector<uint8_t>& s, size_t at) {
    if (s.size() - at < 8) return s.size() - at;
    const std::span<uint8_t> all(s);
    key.Decrypt(0, all.subspan(at, 8));
    const uint16_t inst = static_cast<uint16_t>((s[at] | s[at + 1] << 8) >> 4), type = static_cast<uint16_t>(s[at + 2] | s[at + 3] << 8);
    const uint64_t len = s[at + 4] | s[at + 5] << 8 | s[at + 6] << 16 | uint64_t{s[at + 7]} << 24;
    const size_t end = at + static_cast<size_t>(std::min<uint64_t>(8 + len, s.size() - at));
    size_t pos = at + 8;
    auto piece = [&](size_t n) {
      n = std::min(n, end - pos);
      key.Decrypt(0, all.subspan(pos, n));
      pos += n;
    };
    if (type >= 0xF018 && type <= 0xF117) {
      const bool twoIds = inst == 0x3D5 || inst == 0x217 || inst == 0x543 || inst == 0x46B || inst == 0x6E3 || inst == 0x6E1 ||
                          inst == 0x7A9 || inst == 0x6E5;
      piece(16);
      if (twoIds) piece(16);
      piece(type == 0xF01A || type == 0xF01B || type == 0xF01C ? 34 : 1);
    }
    piece(end - pos);
    return end - at;
  }

  // Decrypts the record at `at` in place, its header first to learn its size; returns its size,
  // header included (the rest of the stream when it doesn't fit).
  static size_t DecryptRecord(const pkg::crypto::Rc4Key& key, uint32_t block, std::vector<uint8_t>& s, size_t at) {
    if (at >= s.size()) return 1;
    if (s.size() - at < 8) return s.size() - at;
    uint8_t head[8];
    std::copy_n(s.begin() + at, 8, head);
    if (!key.Decrypt(block, head)) return s.size() - at;
    const uint64_t len = head[4] | head[5] << 8 | head[6] << 16 | uint64_t{head[7]} << 24;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(8 + len, s.size() - at));
    key.Decrypt(block, std::span<uint8_t>(s).subspan(at, n));
    return n;
  }

  // The document's "___PPT9" tag: its OutlineTextProps9Container pairs an OutlineTextPropsHeaderExAtom
  // (slide id, text type) with that outline text's StyleTextProp9Atom.
  void OutlineStyle9(std::span<const uint8_t> tag) {
    const auto container = Find(tag, 0x0FAE);
    if (!container) return;
    Records rs(container->body);
    std::optional<std::pair<uint32_t, int>> key;
    for (Rec r; rs.Next(r) && style9_.size() < 100000;) {
      if (r.type == 0x0FAF) {
        Bytes b(r.body);
        const uint32_t slide = b.U32();
        const uint32_t type = b.U32();
        key.reset();
        if (b.Ok() && type <= 8) key = std::make_pair(slide, static_cast<int>(type));
      } else if (r.type == 0x0FAC && key) {
        ReadStyle9(r.body, style9_[*key]);
        key.reset();
      }
    }
  }

  // ---- The document

  bool ReadDocument(pkg::Status& status) {
    const Rec document = *Persist(docPersist_);
    Records rs(document.body);
    for (Rec r; rs.Next(r);) {
      Bytes b(r.body);
      switch (r.type) {
        case 0x03E9: {  // DocumentAtom
          slideW_ = b.I32(), slideH_ = b.I32(), notesW_ = b.I32(), notesH_ = b.I32();
          b.Skip(8);
          notesMaster_ = b.U32();
          b.U32();
          firstSlide_ = b.U16();
          break;
        }
        case 0x03F2: Environment(r.body); break;
        case 0x040B:  // DrawingGroup
          // Its default shape properties (FOPT) are only for new shapes: PowerPoint draws a shape that
          // leaves a property out with OfficeArt's own default.
          if (auto dgg = Find(r.body, 0xF000)) dgg_ = dgg->body;
          break;
        case 0x0409: Hyperlinks(r.body); break;
        case 0x0FF0:
          if (r.inst == 0) List(r.body, slides_);
          else if (r.inst == 1) List(r.body, masters_);
          else if (r.inst == 2) List(r.body, notes_);
          break;
        case 0x07D0:  // DocInfoList: its ProgTags
          if (auto tags = Find(r.body, 0x1388)) OutlineStyle9(Pp9Tag(tags->body));
          break;
        case 0x1388: OutlineStyle9(Pp9Tag(r.body)); break;
        default: break;
      }
    }
    // The outline texts' PowerPoint 2000 additions (numbered paragraphs), by slide and text type.
    for (auto* list : {&slides_, &masters_, &notes_})
      for (ListEntry& e : *list)
        for (Text& t : e.texts)
          if (auto it = style9_.find({e.id, t.type}); it != style9_.end()) t.pf9 = it->second;
    if (slideW_ <= 0 || slideH_ <= 0) slideW_ = 5760, slideH_ = 4320;
    if (notesW_ <= 0 || notesH_ <= 0) notesW_ = 4320, notesH_ = 5760;
    if (masters_.empty()) {
      status = pkg::Status::Fail(pkg::Status::Code::Damaged, T("it isn't a PowerPoint presentation or it is damaged."));
      return false;
    }
    return true;
  }

  void Environment(std::span<const uint8_t> body) {
    Records rs(body);
    for (Rec r; rs.Next(r);) {
      Bytes b(r.body);
      if (r.type == 0x07D5) {  // FontCollection
        Records fs(r.body);
        for (Rec f; fs.Next(f) && fonts_.size() < 4096;) {
          // FontEmbedDataBlob: the font before it, embedded (instance: regular, bold, italic, bold italic),
          // the Embedded OpenType a .pptx keeps in ppt/fonts/*.fntdata.
          if (f.type == 0x0FB8 && !fonts_.empty() && f.inst < 4 && !f.body.empty()) embedded_[fonts_.size() - 1][f.inst] = f.body;
          if (f.type != 0x0FB7) continue;
          std::u16string name;
          for (size_t i = 0; i + 1 < std::min<size_t>(f.body.size(), 64); i += 2) {
            const char16_t c = static_cast<char16_t>(f.body[i] | f.body[i + 1] << 8);
            if (!c) break;
            name += c;
          }
          fonts_.push_back(Utf8(name));
        }
      } else if (r.type == 0x0FA4) {  // TextCFExceptionAtom
        ReadCf(b, baseCf_);
      } else if (r.type == 0x0FA5) {  // TextPFExceptionAtom
        b.U16();
        ReadPf(b, basePf_);
      } else if (r.type == 0x0FA3) {  // TextMasterStyleAtom: the default style of other text
        MasterStyle m;
        if (r.inst < 9 && ReadMasterStyle(r, m)) envStyles_[r.inst] = m;
      }
    }
  }

  void Hyperlinks(std::span<const uint8_t> body) {
    Records rs(body);
    for (Rec r; rs.Next(r) && hyperlinks_.size() < 10000;) {
      if (r.type != 0x0FD7) continue;  // ExHyperlinkContainer
      uint32_t id = 0;
      std::u16string target, location;
      Records hs(r.body);
      for (Rec h; hs.Next(h);) {
        if (h.type == 0x0FD3) {
          Bytes b(h.body);
          id = b.U32();
        } else if (h.type == 0x0FBA && (h.inst == 1 || h.inst == 3)) {
          std::u16string s;
          for (size_t i = 0; i + 1 < h.body.size() && s.size() < 2048; i += 2) s += static_cast<char16_t>(h.body[i] | h.body[i + 1] << 8);
          (h.inst == 1 ? target : location) = s;
        }
      }
      std::string t = Utf8(target);
      if (t.empty() && !location.empty()) continue;  // a jump inside the deck: not kept yet
      if (!location.empty()) t += "#" + Utf8(location);
      if (!t.empty()) hyperlinks_[id] = t;
    }
  }
  static std::string Utf8(std::u16string_view s) {
    std::string e = Esc(s), out;
    for (size_t i = 0; i < e.size(); ++i) {
      if (e.compare(i, 5, "&amp;") == 0) out += '&', i += 4;
      else if (e.compare(i, 4, "&lt;") == 0) out += '<', i += 3;
      else if (e.compare(i, 4, "&gt;") == 0) out += '>', i += 3;
      else if (e.compare(i, 6, "&quot;") == 0) out += '"', i += 5;
      else out += e[i];
    }
    return out;
  }

  void List(std::span<const uint8_t> body, std::vector<ListEntry>& out) {
    Records rs(body);
    std::span<const uint8_t> textStart;
    size_t at = 0, begin = 0;
    const auto flush = [&](size_t end) {
      if (out.empty() || textStart.empty() || out.back().texts.size() >= 4096) return;
      Text t;
      ReadText(body.subspan(begin, end - begin), t, [this](uint32_t id) { return Link(id); });
      out.back().texts.push_back(std::move(t));
    };
    for (Rec r; rs.Next(r) && out.size() < 100000;) {
      const size_t next = at + 8 + r.body.size();
      if (r.type == 0x03F3) {  // SlidePersistAtom
        flush(at);
        textStart = {};
        Bytes b(r.body);
        ListEntry e;
        e.persist = b.U32();
        e.flags = b.U32();
        b.U32();
        e.id = b.U32();
        out.push_back(std::move(e));
      } else if (r.type == 0x0F9F) {  // TextHeaderAtom: a new text
        flush(at);
        textStart = r.body;
        begin = at;
      }
      at = next;
    }
    flush(at);
  }

  std::string Link(uint32_t id) const {
    const auto it = hyperlinks_.find(id);
    return it == hyperlinks_.end() ? std::string() : it->second;
  }

  // ---- Pictures: the BLIP store's entries, from the Pictures stream or inside the store.

  bool Picture(uint32_t pib, std::string& ext, std::vector<uint8_t>& bytes) const {
    const auto store = Find(dgg_, 0xF001);
    if (!store || pib == 0) return false;
    Records rs(store->body);
    uint32_t i = 0;
    for (Rec r; rs.Next(r);) {
      if (++i != pib) continue;
      if (r.type != 0xF007 || r.body.size() < 36) return false;
      Bytes b(r.body);
      b.Skip(28);
      const uint32_t delay = b.U32();
      b.U8();
      const uint8_t nameLength = b.U8();
      const size_t inside = 36 + nameLength;
      if (r.body.size() > inside + 8) return draw::DecodeBlip(r.body.subspan(inside), ext, bytes);
      if (delay >= pictures_.size()) return false;
      return draw::DecodeBlip(std::span<const uint8_t>(pictures_).subspan(delay), ext, bytes);
    }
    return false;
  }

  // ---- Writing

  struct Part {
    std::string name, text;
  };

  struct Master {
    uint32_t id = 0;
    size_t index = 0;  // in masters_
    Context cx;
    std::string background;
    int part = 0;          // slideMasterN
    int defaultLayout = 0;  // slideLayoutN
    std::vector<int> layouts;
  };

  Context ContextFor(std::span<const uint8_t> body, const Master* master) const {
    Context cx;
    cx.fonts = &fonts_;
    cx.basePf = basePf_;
    cx.baseCf = baseCf_;
    if (master) cx = master->cx;
    // The slide's own scheme, unless it follows the master's.
    if (auto atom = Find(body, 0x03EF)) {
      Bytes b(atom->body);
      b.Skip(20);
      const uint16_t flags = b.U16();
      if (!(flags & 2) || !master)
        if (auto scheme = Find(body, 0x07F0, 1)) cx.scheme = ReadScheme(scheme->body);
    } else if (auto scheme = Find(body, 0x07F0, 1)) {
      cx.scheme = ReadScheme(scheme->body);
    }
    return cx;
  }

  DrawingEnv EnvFor(const Context& cx, Rels& rels, const ListEntry* outline, bool master) {
    DrawingEnv env;
    env.text = cx;
    env.masters = &masterShapes_;
    env.master = master;
    env.picture = [this, &rels](uint32_t pib) -> std::string {
      const std::string target = Media(pib);
      return target.empty() ? std::string() : rels.Add("image", "../media/" + target);
    };
    env.link = [&rels](const std::string& target) -> std::string {
      if (target.rfind("ppaction:", 0) == 0) return "\x01" + target;
      return rels.Add("hyperlink", target, true);
    };
    env.hyperlink = [this](uint32_t id) { return Link(id); };
    env.outline = [outline](int32_t index) -> const Text* {
      if (!outline || index < 0 || static_cast<size_t>(index) >= outline->texts.size()) return nullptr;
      return &outline->texts[index];
    };
    return env;
  }

  // A picture's media part name ("imageN.ext"), made once per BLIP store entry.
  std::string Media(uint32_t pib) {
    if (auto it = media_.find(pib); it != media_.end()) return it->second;
    std::string ext;
    std::vector<uint8_t> bytes;
    std::string name;
    if (media_.size() < 10000 && Picture(pib, ext, bytes)) {
      name = "image" + Num(static_cast<long long>(mediaParts_.size()) + 1) + "." + ext;
      mediaParts_.push_back({name, std::move(bytes)});
      mediaExts_.insert(ext);
    }
    media_.emplace(pib, name);
    return name;
  }

  static std::string Background(const std::string& fill) {
    return "<p:bg><p:bgPr>" + (fill.empty() ? std::string("<a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill>") : fill) +
           "<a:effectLst/></p:bgPr></p:bg>";
  }

  std::string ThemeXml(const Context& cx, const std::string& name) const {
    const auto& c = cx.scheme.rgb;
    const auto clr = [](const char* tag, uint32_t rgb) {
      return std::string("<a:") + tag + ">" + ColorXml(rgb) + "</a:" + tag + ">";
    };
    const auto font = [&](int type) {
      if (cx.styles[type] && cx.styles[type]->levels[0] && cx.styles[type]->levels[0]->second.Has(kTypeface)) {
        const uint16_t f = cx.styles[type]->levels[0]->second.font;
        if (f < fonts_.size()) return Esc(fonts_[f]);
      }
      if (baseCf_.Has(kTypeface) && baseCf_.font < fonts_.size()) return Esc(fonts_[baseCf_.font]);
      return std::string("Arial");
    };
    const std::string solid = "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>";
    const std::string line = "<a:ln w=\"9525\" cap=\"flat\" cmpd=\"sng\" algn=\"ctr\">" + solid + "<a:prstDash val=\"solid\"/></a:ln>";
    return std::string(kDecl) + "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"" + Esc(name) +
           "\"><a:themeElements><a:clrScheme name=\"" + Esc(name) + "\">" + clr("dk1", c[1]) + clr("lt1", c[0]) + clr("dk2", c[3]) +
           clr("lt2", c[2]) + clr("accent1", c[4]) + clr("accent2", c[5]) + clr("accent3", c[6]) + clr("accent4", c[7]) +
           clr("accent5", c[4]) + clr("accent6", c[5]) + clr("hlink", c[6]) + clr("folHlink", c[7]) +
           "</a:clrScheme><a:fontScheme name=\"" + Esc(name) + "\"><a:majorFont><a:latin typeface=\"" + font(kTitle) +
           "\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont><a:minorFont><a:latin typeface=\"" + font(kBody) +
           "\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont></a:fontScheme><a:fmtScheme name=\"" + Esc(name) +
           "\"><a:fillStyleLst>" + solid + solid + solid + "</a:fillStyleLst><a:lnStyleLst>" + line + line + line +
           "</a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle>"
           "<a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst>" +
           solid + solid + solid + "</a:bgFillStyleLst></a:fmtScheme></a:themeElements><a:objectDefaults/><a:extraClrSchemeLst/></a:theme>";
  }

  // The XML part `name` of the package in a round-trip atom (PowerPoint 2007 or later data) of type
  // `type` in `container`, into `doc`; false when there's none.
  static bool RoundTripXml(std::span<const uint8_t> container, uint16_t type, std::string_view name, pugi::xml_document& doc) {
    const auto atom = Find(container, type);
    return atom && PackageXml(atom->body, name, doc);
  }

  // A PowerPoint 2007 or later master's color mapping (RoundTripColorMapping12Atom: an a:clrMap) as
  // p:clrMap's attributes; empty when there's none or it isn't one.
  static std::string RoundTripClrMap(std::span<const uint8_t> master) {
    const auto atom = Find(master, 0x040F);
    pugi::xml_document doc;
    if (!atom || !doc.load_buffer(atom->body.data(), atom->body.size())) return {};
    static constexpr const char* kKeys[] = {"bg1", "tx1", "bg2", "tx2", "accent1", "accent2", "accent3",
                                            "accent4", "accent5", "accent6", "hlink", "folHlink"};
    std::string out;
    for (const char* key : kKeys) {
      const std::string_view v = doc.document_element().attribute(key).value();
      bool known = v == "dk1" || v == "lt1" || v == "dk2" || v == "lt2";
      for (const char* k : kKeys) known |= std::string_view(k).size() > 3 && v == k;  // accentN, hlink, folHlink
      if (!known) return {};
      out += std::string(out.empty() ? "" : " ") + key + "=\"" + std::string(v) + "\"";
    }
    return out;
  }

  // A PowerPoint 2007 or later master's own theme (RoundTripTheme12Atom's package), with the pictures
  // it uses as media parts and `rels` its relationships. Empty when there's none, or when it refers to
  // anything it doesn't carry.
  std::string RoundTripTheme(std::span<const uint8_t> master, std::string& rels) {
    rels.clear();
    const auto atom = Find(master, 0x040E);
    if (!atom || atom->body.size() < 22) return {};
    auto snapshot = pkg::Snapshot::FromBytes(std::vector<uint8_t>(atom->body.begin(), atom->body.end()));
    pkg::ZipReader zip;
    if (!snapshot || !zip.Open(*snapshot).Ok()) return {};
    const auto read = [&zip](const std::string& name, std::vector<uint8_t>& bytes) {
      for (const pkg::ZipEntry& e : zip.Entries())
        if (e.name == name) return e.uncompressedSize <= (16u << 20) && zip.Read(e, bytes).Ok();
      return false;
    };
    for (const pkg::ZipEntry& e : zip.Entries()) {
      constexpr std::string_view kPrefix = "theme/theme/theme";
      if (e.name.rfind(kPrefix, 0) != 0 || e.name.size() < 4 || e.name.substr(e.name.size() - 4) != ".xml") continue;
      std::vector<uint8_t> bytes;
      if (e.uncompressedSize > (8u << 20) || !zip.Read(e, bytes).Ok()) continue;
      pugi::xml_document doc;
      if (!doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8)) continue;
      const std::string_view root = doc.document_element().name();
      if (root.size() < 5 || root.substr(root.size() - 5) != "theme") continue;
      // Its relationships: pictures become media parts of the deck; anything else, and any reference
      // without a relationship, and the theme isn't used.
      std::set<std::string> ids;
      Rels out;
      std::string relsXml;
      std::vector<uint8_t> relsBytes;
      const std::string relsName = "theme/theme/_rels/" + e.name.substr(12) + ".rels";
      if (read(relsName, relsBytes)) {
        pugi::xml_document r;
        if (!r.load_buffer(relsBytes.data(), relsBytes.size())) return {};
        for (pugi::xml_node rel : r.document_element().children()) {
          const std::string id = rel.attribute("Id").as_string(), type = rel.attribute("Type").as_string(),
                            target = rel.attribute("Target").as_string();
          if (type.size() < 6 || type.substr(type.size() - 6) != "/image" || std::string_view(rel.attribute("TargetMode").as_string()) == "External")
            return {};
          // "../media/image1.jpeg" from theme/theme/.
          if (target.rfind("../media/", 0) != 0 || target.find("..", 3) != std::string::npos) return {};
          std::vector<uint8_t> picture;
          if (!read("theme/media/" + target.substr(9), picture) || picture.empty()) return {};
          std::string ext = target.substr(target.find_last_of('.') + 1);
          for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (ext != "png" && ext != "jpeg" && ext != "jpg" && ext != "gif" && ext != "bmp" && ext != "tiff" && ext != "emf" && ext != "wmf") return {};
          const std::string name = "image" + Num(static_cast<long long>(mediaParts_.size()) + 1) + "." + ext;
          mediaParts_.push_back({name, std::move(picture)});
          mediaExts_.insert(ext);
          relsXml += "<Relationship Id=\"" + Esc(id) + "\" Type=\"" + Esc(type) + "\" Target=\"../media/" + name + "\"/>";
          ids.insert(id);
        }
      }
      bool known = true;
      const std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node n) {
        for (pugi::xml_attribute a : n.attributes()) {
          const std::string_view an = a.name();
          if (an.rfind("r:", 0) == 0 && !ids.count(a.value())) known = false;
        }
        for (pugi::xml_node c : n.children()) walk(c);
      };
      walk(doc.document_element());
      if (!known) return {};
      if (!relsXml.empty())
        rels = std::string(kDecl) + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" + relsXml +
               "</Relationships>";
      return std::string(bytes.begin(), bytes.end());
    }
    return {};
  }

  std::vector<uint8_t> Write(pkg::Status& status) {
    std::vector<Part> parts;
    std::string overrides;
    const auto add = [&](std::string name, std::string text, const char* type) {
      if (type) overrides += "<Override PartName=\"/" + name + "\" ContentType=\"" + type + "\"/>";
      parts.push_back({std::move(name), std::move(text)});
    };
    Rels presRels;
    std::string masterIds, slideIds;
    uint32_t nextLayoutId = 2147483648u;
    int themes = 0, layouts = 0;

    // The masters' shapes, which shapes on slides inherit from.
    for (const ListEntry& e : masters_)
      if (const auto rec = Persist(e.persist); rec && (rec->type == 0x03F8 || rec->type == 0x03EE))
        if (auto drawing = Find(rec->body, 0x040C)) IndexShapes(drawing->body, masterShapes_);
    if (const auto rec = Persist(notesMaster_); rec && rec->type == 0x03F0)
      if (auto drawing = Find(rec->body, 0x040C)) IndexShapes(drawing->body, masterShapes_);

    // Main masters first, each with a layout of its own; title masters become layouts of theirs.
    std::vector<Master> mains;
    std::map<uint32_t, std::pair<size_t, int>> layoutOf;  // master list id -> main master index, layout number
    for (size_t i = 0; i < masters_.size() && mains.size() < 1000; ++i) {
      const auto rec = Persist(masters_[i].persist);
      if (!rec || rec->type != 0x03F8) continue;
      Master m;
      m.id = masters_[i].id;
      m.index = i;
      m.cx.fonts = &fonts_;
      m.cx.basePf = basePf_;
      m.cx.baseCf = baseCf_;
      for (int t = 0; t < 9; ++t) m.cx.styles[t] = envStyles_[t];
      m.cx.docOther = envStyles_[kOther];
      Records rs(rec->body);
      for (Rec r; rs.Next(r);) {
        if (r.type == 0x0FA3 && r.inst < 9) {
          MasterStyle st;
          if (ReadMasterStyle(r, st)) m.cx.styles[r.inst] = st;
        } else if (r.type == 0x07F0 && r.inst == 1) {
          m.cx.scheme = ReadScheme(r.body);
        }
      }
      if (pugi::xml_document ts; RoundTripXml(rec->body, 0x0423, "drs/slideMasters/slideMaster1.xml", ts))
        for (int t = 0; t < 3; ++t)
          for (int l = 0; l < 5; ++l) {
            static constexpr const char* kStyle[] = {"p:titleStyle", "p:bodyStyle", "p:otherStyle"};
            const pugi::xml_node d = ts.document_element().child(kStyle[t]).child(("a:lvl" + Num(l + 1) + "pPr").c_str()).child("a:defRPr");
            m.cx.kern[t][l] = std::clamp(d.attribute("kern").as_int(), 0, 400000);
          }
      m.part = static_cast<int>(mains.size()) + 1;
      m.defaultLayout = ++layouts;
      layoutOf[m.id] = {mains.size(), m.defaultLayout};
      mains.push_back(std::move(m));
    }
    if (mains.empty()) {
      status = pkg::Status::Fail(pkg::Status::Code::Damaged, T("it isn't a PowerPoint presentation or it is damaged."));
      return {};
    }
    std::vector<std::pair<size_t, size_t>> titleMasters;  // masters_ index, main master index
    for (size_t i = 0; i < masters_.size(); ++i) {
      const auto rec = Persist(masters_[i].persist);
      if (!rec || rec->type != 0x03EE) continue;
      uint32_t mainId = 0;
      if (auto atom = Find(rec->body, 0x03EF)) {
        Bytes b(atom->body);
        b.Skip(12);
        mainId = b.U32();
      }
      const auto it = layoutOf.find(mainId);
      const size_t main = it != layoutOf.end() ? it->second.first : 0;
      const int number = ++layouts;
      layoutOf[masters_[i].id] = {main, number};
      mains[main].layouts.push_back(number);
      titleMasters.emplace_back(i, main);
    }

    // Masters and their themes and default layouts. Master ids come first (2147483648 + n - 1),
    // layout ids after them: all of them unique.
    nextLayoutId = 2147483648u + static_cast<uint32_t>(mains.size()) - 1;
    for (Master& m : mains) {
      const Rec rec = *Persist(masters_[m.index].persist);
      Rels rels;
      std::string layoutIds;
      std::vector<int> all = {m.defaultLayout};
      all.insert(all.end(), m.layouts.begin(), m.layouts.end());
      for (int l : all) layoutIds += "<p:sldLayoutId id=\"" + Num(++nextLayoutId) + "\" r:id=\"" + rels.Add("slideLayout", "../slideLayouts/slideLayout" + Num(l) + ".xml") + "\"/>";
      const int theme = ++themes;
      std::string themeRels;
      std::string themeXml = RoundTripTheme(rec.body, themeRels), clrMap = kClrMap;
      if (themeXml.empty()) themeXml = ThemeXml(m.cx, "Master " + Num(m.part));
      else if (std::string own = RoundTripClrMap(rec.body); !own.empty()) clrMap = std::move(own);  // with its own theme only
      add("ppt/theme/theme" + Num(theme) + ".xml", std::move(themeXml), "application/vnd.openxmlformats-officedocument.theme+xml");
      if (!themeRels.empty()) parts.push_back({"ppt/theme/_rels/theme" + Num(theme) + ".xml.rels", std::move(themeRels)});
      rels.Add("theme", "../theme/theme" + Num(theme) + ".xml");
      DrawingEnv env = EnvFor(m.cx, rels, nullptr, true);
      DrawingXmlOut d;
      if (auto drawing = Find(rec.body, 0x040C)) d = DrawingXml(drawing->body, env);
      m.background = d.background;
      std::string xml = std::string(kDecl) + "<p:sldMaster" + kNs + "><p:cSld>" + Background(d.background) + "<p:spTree>" + kTree + d.shapes +
                        "</p:spTree></p:cSld><p:clrMap " + clrMap + "/><p:sldLayoutIdLst>" + layoutIds + "</p:sldLayoutIdLst><p:txStyles><p:titleStyle>" +
                        LevelStylesXml(m.cx, kTitle) + "</p:titleStyle><p:bodyStyle>" + LevelStylesXml(m.cx, kBody) +
                        "</p:bodyStyle><p:otherStyle>" + LevelStylesXml(m.cx, kOther) + "</p:otherStyle></p:txStyles></p:sldMaster>";
      const std::string name = "ppt/slideMasters/slideMaster" + Num(m.part) + ".xml";
      add(name, std::move(xml), "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml");
      parts.push_back({"ppt/slideMasters/_rels/slideMaster" + Num(m.part) + ".xml.rels", rels.Xml()});
      masterIds += "<p:sldMasterId id=\"" + Num(2147483648u + static_cast<uint32_t>(m.part) - 1) + "\" r:id=\"" +
                   presRels.Add("slideMaster", "slideMasters/slideMaster" + Num(m.part) + ".xml") + "\"/>";
      // Its own layout: nothing of its own, so slides show the master as it is.
      Rels lr;
      lr.Add("slideMaster", "../slideMasters/slideMaster" + Num(m.part) + ".xml");
      add("ppt/slideLayouts/slideLayout" + Num(m.defaultLayout) + ".xml",
          std::string(kDecl) + "<p:sldLayout" + kNs + " preserve=\"1\"><p:cSld name=\"Master " + Num(m.part) + "\"><p:spTree>" + kTree +
              "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>",
          "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml");
      parts.push_back({"ppt/slideLayouts/_rels/slideLayout" + Num(m.defaultLayout) + ".xml.rels", lr.Xml()});
    }
    // Title masters: layouts with their shapes.
    for (const auto& [index, main] : titleMasters) {
      const Rec rec = *Persist(masters_[index].persist);
      const int number = layoutOf[masters_[index].id].second;
      Rels rels;
      rels.Add("slideMaster", "../slideMasters/slideMaster" + Num(mains[main].part) + ".xml");
      const Context cx = ContextFor(rec.body, &mains[main]);
      DrawingEnv env = EnvFor(cx, rels, &masters_[index], true);
      DrawingXmlOut d;
      if (auto drawing = Find(rec.body, 0x040C)) d = DrawingXml(drawing->body, env);
      uint16_t flags = 7;
      if (auto atom = Find(rec.body, 0x03EF)) {
        Bytes b(atom->body);
        b.Skip(20);
        flags = b.U16();
      }
      const std::string bg = flags & 4 || d.background.empty() ? std::string() : Background(d.background);
      add("ppt/slideLayouts/slideLayout" + Num(number) + ".xml",
          std::string(kDecl) + "<p:sldLayout" + kNs + (flags & 1 ? "" : " showMasterSp=\"0\"") + " type=\"title\" preserve=\"1\"><p:cSld name=\"Title Master\">" +
              bg + "<p:spTree>" + kTree + d.shapes + "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>",
          "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml");
      parts.push_back({"ppt/slideLayouts/_rels/slideLayout" + Num(number) + ".xml.rels", rels.Xml()});
    }

    // The notes master.
    std::string notesMasterIds;
    std::map<uint32_t, size_t> notesById;
    for (size_t i = 0; i < notes_.size(); ++i) notesById.emplace(notes_[i].id, i);
    bool haveNotesMaster = false;
    if (const auto rec = Persist(notesMaster_); rec && rec->type == 0x03F0) {
      haveNotesMaster = true;
      Rels rels;
      const int theme = ++themes;
      Context cx = ContextFor(rec->body, &mains[0]);
      add("ppt/theme/theme" + Num(theme) + ".xml", ThemeXml(cx, "Notes"), "application/vnd.openxmlformats-officedocument.theme+xml");
      rels.Add("theme", "../theme/theme" + Num(theme) + ".xml");
      DrawingEnv env = EnvFor(cx, rels, nullptr, true);
      env.notes = true;
      DrawingXmlOut d;
      if (auto drawing = Find(rec->body, 0x040C)) d = DrawingXml(drawing->body, env);
      add("ppt/notesMasters/notesMaster1.xml",
          std::string(kDecl) + "<p:notesMaster" + kNs + "><p:cSld>" + Background(d.background) + "<p:spTree>" + kTree + d.shapes +
              "</p:spTree></p:cSld><p:clrMap " + kClrMap + "/><p:notesStyle>" + LevelStylesXml(cx, kNotes) + "</p:notesStyle></p:notesMaster>",
          "application/vnd.openxmlformats-officedocument.presentationml.notesMaster+xml");
      parts.push_back({"ppt/notesMasters/_rels/notesMaster1.xml.rels", rels.Xml()});
      notesMasterIds = "<p:notesMasterIdLst><p:notesMasterId r:id=\"" + presRels.Add("notesMaster", "notesMasters/notesMaster1.xml") +
                       "\"/></p:notesMasterIdLst>";
    }

    // Slides, with their notes.
    int slideNumber = 0, notesNumber = 0;
    for (const ListEntry& entry : slides_) {
      const auto rec = Persist(entry.persist);
      if (!rec || rec->type != 0x03EE || slideNumber >= 100000) continue;
      const int n = ++slideNumber;
      uint32_t masterId = 0, notesId = 0;
      uint16_t flags = 7;
      if (auto atom = Find(rec->body, 0x03EF)) {
        Bytes b(atom->body);
        b.Skip(12);
        masterId = b.U32();
        notesId = b.U32();
        flags = b.U16();
      }
      const auto lay = layoutOf.find(masterId);
      const size_t main = lay != layoutOf.end() ? lay->second.first : 0;
      const int layout = lay != layoutOf.end() ? lay->second.second : mains[0].defaultLayout;
      Rels rels;
      rels.Add("slideLayout", "../slideLayouts/slideLayout" + Num(layout) + ".xml");
      const Context cx = ContextFor(rec->body, &mains[main]);
      DrawingEnv env = EnvFor(cx, rels, &entry, false);
      DrawingXmlOut d;
      if (auto drawing = Find(rec->body, 0x040C)) d = DrawingXml(drawing->body, env);
      bool hidden = false;
      if (auto info = Find(rec->body, 0x03F9)) {
        Bytes b(info->body);
        b.Skip(10);
        hidden = b.U16() & 4;
      }
      const std::string bg = flags & 4 || d.background.empty() ? std::string() : Background(d.background);
      // Its notes page.
      if (const auto nt = notesById.find(notesId); haveNotesMaster && notesId && nt != notesById.end()) {
        if (const auto nrec = Persist(notes_[nt->second].persist); nrec && nrec->type == 0x03F0) {
          const int nn = ++notesNumber;
          Rels nrels;
          nrels.Add("notesMaster", "../notesMasters/notesMaster1.xml");
          nrels.Add("slide", "../slides/slide" + Num(n) + ".xml");
          const Context ncx = ContextFor(nrec->body, &mains[0]);
          DrawingEnv nenv = EnvFor(ncx, nrels, &notes_[nt->second], false);
          nenv.notes = true;
          DrawingXmlOut nd;
          if (auto drawing = Find(nrec->body, 0x040C)) nd = DrawingXml(drawing->body, nenv);
          add("ppt/notesSlides/notesSlide" + Num(nn) + ".xml",
              std::string(kDecl) + "<p:notes" + kNs + "><p:cSld><p:spTree>" + kTree + nd.shapes +
                  "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:notes>",
              "application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml");
          parts.push_back({"ppt/notesSlides/_rels/notesSlide" + Num(nn) + ".xml.rels", nrels.Xml()});
          rels.Add("notesSlide", "../notesSlides/notesSlide" + Num(nn) + ".xml");
        }
      }
      add("ppt/slides/slide" + Num(n) + ".xml",
          std::string(kDecl) + "<p:sld" + kNs + (flags & 1 ? "" : " showMasterSp=\"0\"") + (hidden ? " show=\"0\"" : "") + "><p:cSld>" + bg +
              "<p:spTree>" + kTree + d.shapes + "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>",
          "application/vnd.openxmlformats-officedocument.presentationml.slide+xml");
      parts.push_back({"ppt/slides/_rels/slide" + Num(n) + ".xml.rels", rels.Xml()});
      slideIds += "<p:sldId id=\"" + Num(255 + n) + "\" r:id=\"" + presRels.Add("slide", "slides/slide" + Num(n) + ".xml") + "\"/>";
    }

    // The presentation.
    presRels.Add("theme", "theme/theme1.xml");
    presRels.Add("presProps", "presProps.xml");
    presRels.Add("viewProps", "viewProps.xml");
    presRels.Add("tableStyles", "tableStyles.xml");
    Context defaults;
    defaults.fonts = &fonts_;
    defaults.basePf = basePf_;
    defaults.baseCf = baseCf_;
    defaults.scheme = mains[0].cx.scheme;
    defaults.kern[2] = mains[0].cx.kern[2];  // text boxes kern as the first master's other text (when it was saved with any)
    for (int t = 0; t < 9; ++t) defaults.styles[t] = envStyles_[t];
    defaults.docOther = envStyles_[kOther];
    const long long cx = std::clamp<long long>(std::llround(slideW_ * kEmuPerUnit), 914400, 51206400);
    const long long cy = std::clamp<long long>(std::llround(slideH_ * kEmuPerUnit), 914400, 51206400);
    const long long ncx = std::clamp<long long>(std::llround(notesW_ * kEmuPerUnit), 1, 51206400);
    const long long ncy = std::clamp<long long>(std::llround(notesH_ * kEmuPerUnit), 1, 51206400);
    // Embedded fonts, as the .pptx keeps them.
    std::string embedded;
    std::vector<std::pair<std::string, std::span<const uint8_t>>> fontParts;
    for (const auto& [index, styles] : embedded_) {
      std::string faces;
      static const char* const kStyle[] = {"regular", "bold", "italic", "boldItalic"};
      for (int k = 0; k < 4; ++k) {
        if (styles[k].empty()) continue;
        const std::string name = "fonts/font" + Num(fontParts.size() + 1) + ".fntdata";
        faces += std::string("<p:") + kStyle[k] + " r:id=\"" + presRels.Add("font", name) + "\"/>";
        fontParts.emplace_back("ppt/" + name, styles[k]);
      }
      if (!faces.empty()) embedded += "<p:embeddedFont><p:font typeface=\"" + Esc(fonts_[index]) + "\"/>" + faces + "</p:embeddedFont>";
    }
    if (!embedded.empty()) embedded = "<p:embeddedFontLst>" + embedded + "</p:embeddedFontLst>";
    add("ppt/presentation.xml",
        std::string(kDecl) + "<p:presentation" + kNs + (firstSlide_ > 1 && firstSlide_ < 10000 ? " firstSlideNum=\"" + Num(firstSlide_) + "\"" : "") +
            " saveSubsetFonts=\"1\"><p:sldMasterIdLst>" + masterIds + "</p:sldMasterIdLst>" + notesMasterIds +
            (slideIds.empty() ? "" : "<p:sldIdLst>" + slideIds + "</p:sldIdLst>") + "<p:sldSz cx=\"" + Num(cx) + "\" cy=\"" + Num(cy) +
            "\"/><p:notesSz cx=\"" + Num(ncx) + "\" cy=\"" + Num(ncy) + "\"/>" + embedded + "<p:defaultTextStyle>" + LevelStylesXml(defaults, kOther) +
            "</p:defaultTextStyle></p:presentation>",
        "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml");
    parts.push_back({"ppt/_rels/presentation.xml.rels", presRels.Xml()});
    add("ppt/presProps.xml", std::string(kDecl) + "<p:presentationPr" + kNs + "/>", "application/vnd.openxmlformats-officedocument.presentationml.presProps+xml");
    add("ppt/viewProps.xml", std::string(kDecl) + "<p:viewPr" + kNs + "><p:gridSpacing cx=\"76200\" cy=\"76200\"/></p:viewPr>",
        "application/vnd.openxmlformats-officedocument.presentationml.viewProps+xml");
    add("ppt/tableStyles.xml",
        std::string(kDecl) + "<a:tblStyleLst xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" def=\"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}\"/>",
        "application/vnd.openxmlformats-officedocument.presentationml.tableStyles+xml");
    parts.push_back({"_rels/.rels", std::string(kDecl) +
                                        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
                                        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
                                        "Target=\"ppt/presentation.xml\"/></Relationships>"});
    std::string types = std::string(kDecl) +
                        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" "
                        "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>";
    for (const std::string& ext : mediaExts_) {
      const char* type = ext == "png" ? "image/png" : ext == "jpeg" || ext == "jpg" ? "image/jpeg" : ext == "gif" ? "image/gif"
                         : ext == "bmp" ? "image/bmp" : ext == "tiff" ? "image/tiff" : ext == "emf" ? "image/x-emf" : "image/x-wmf";
      types += "<Default Extension=\"" + ext + "\" ContentType=\"" + type + "\"/>";
    }
    if (!fontParts.empty()) types += "<Default Extension=\"fntdata\" ContentType=\"application/x-fontdata\"/>";
    parts.insert(parts.begin(), {"[Content_Types].xml", types + overrides + "</Types>"});

    pkg::MemorySink sink;
    pkg::ZipWriter zip(sink);
    for (const Part& p : parts)
      if (!zip.Add(p.name, std::span(reinterpret_cast<const uint8_t*>(p.text.data()), p.text.size())).Ok()) {
        status = pkg::Status::Fail(pkg::Status::Code::TooLarge, T("it is too large to open."));
        return {};
      }
    for (const auto& [name, bytes] : mediaParts_)
      if (!zip.Add("ppt/media/" + name, bytes, nullptr, pkg::ZipWriter::Compression::Store).Ok()) {
        status = pkg::Status::Fail(pkg::Status::Code::TooLarge, T("it is too large to open."));
        return {};
      }
    for (const auto& [name, bytes] : fontParts)
      if (!zip.Add(name, bytes).Ok()) {
        status = pkg::Status::Fail(pkg::Status::Code::TooLarge, T("it is too large to open."));
        return {};
      }
    if (!zip.Finish().Ok()) {
      status = pkg::Status::Fail(pkg::Status::Code::TooLarge, T("it is too large to open."));
      return {};
    }
    return std::move(sink.bytes);
  }

  std::vector<uint8_t> doc_, pictures_;
  std::map<uint32_t, uint32_t> persist_;
  std::map<std::pair<uint32_t, int>, std::vector<Pf9>> style9_;  // outline texts' PF9, by slide id and text type
  std::map<size_t, std::array<std::span<const uint8_t>, 4>> embedded_;  // fonts_ index: regular, bold, italic, bold italic
  uint32_t docPersist_ = 0, encryptPersist_ = 0;
  int32_t slideW_ = 0, slideH_ = 0, notesW_ = 0, notesH_ = 0;
  uint32_t notesMaster_ = 0;
  int firstSlide_ = 1;
  std::vector<std::string> fonts_;
  Pf basePf_;
  Cf baseCf_;
  std::array<std::optional<MasterStyle>, 9> envStyles_;
  std::span<const uint8_t> dgg_;
  ShapeIndex masterShapes_;
  std::map<uint32_t, std::string> hyperlinks_;
  std::vector<ListEntry> masters_, slides_, notes_;
  std::map<uint32_t, std::string> media_;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> mediaParts_;
  std::set<std::string> mediaExts_;
};

}  // namespace

std::vector<uint8_t> ToPptx(const StreamFn& streams, pkg::Status& status, std::wstring_view password) {
  status = {};
  Converter c;
  c.password = password;
  return c.Convert(streams, status);
}

bool NeedsPassword(const StreamFn& streams) {
  pkg::Status status;
  Converter c;
  c.stopAtEncryption = true;
  c.Convert(streams, status);
  return c.encrypted;
}

bool IsPresentation(const StreamFn& streams) { return streams("PowerPoint Document").has_value(); }

}  // namespace fw::slide::ppt

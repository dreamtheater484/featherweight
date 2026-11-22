#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Painter.h"

// Text in slides, resolved (plan §8.3): every paragraph and run of a text body with the properties
// it ends up with after inheritance, from the presentation's default text style through the
// master's text styles, the master's and the layout's placeholder, the shape's own list style and
// style reference, down to the paragraph and the run. Colors are resolved against the theme and the
// color map of the slide shown. The renderer lays these out with fwtext; it never reads a:pPr or
// a:rPr itself.
namespace fw::slide {

// What a click (or a hover) on a shape or a run does: a:hlinkClick, a:hlinkHover.
struct Hyperlink {
  enum class Action : uint8_t {
    None,        // no link, or ppaction://noaction
    Url,         // a web or mail address (url); the view filters it as PDF links are
    Slide,       // jump to a slide of this deck (slide)
    FirstSlide,  // ppaction://hlinkshowjump?jump=firstslide
    LastSlide,
    NextSlide,
    PreviousSlide,
    LastViewed,  // the slide shown before this one
    EndShow,
    CustomShow,  // play a custom show (customShow), and come back when returns is set
    File,        // another file (url, a file: target); never opened during a show in P1
    Program,     // ppaction://program: never run (plan §4.7)
    Macro,       // ppaction://macro: never run
    Ole,         // ppaction://ole: never started
    Other,       // something else; does nothing
  };
  Action action = Action::None;
  std::string url;           // Url and File: the relationship's target, as written
  int slide = -1;            // Slide: the slide's index in the deck; -1 when it isn't in it
  uint32_t customShow = 0;   // CustomShow: the custom show's id
  bool returns = false;      // CustomShow: return=true
  std::string tooltip;
  std::string actionText;    // the action attribute as written ("ppaction://hlinksldjump")
  bool highlightClick = false;
  bool endSound = false;
  std::string soundRel;      // a:snd r:embed: the sound to play, in the part's relationships

  explicit operator bool() const { return action != Action::None; }
};

enum class TextAlign : uint8_t { Left, Center, Right, Justify, JustifyLow, Distributed, ThaiDistributed };
enum class FontAlign : uint8_t { Auto, Top, Center, Baseline, Bottom };

// Line spacing (a:lnSpc) and the space before and after a paragraph (a:spcBef, a:spcAft).
struct Spacing {
  bool percent = false;  // true: `value` is a fraction (1 = 100 %); for lnSpc of single spacing,
                         // for spcBef and spcAft of the paragraph's first line's font size
  float value = 0;       // false: points
};

struct TabStop {
  enum class Kind : uint8_t { Left, Center, Right, Decimal };
  float position = 0;  // points from the text box's left inset
  Kind kind = Kind::Left;
};

struct Bullet {
  enum class Kind : uint8_t { None, Char, AutoNumber, Picture };
  Kind kind = Kind::None;
  std::wstring character;        // Char
  std::string scheme;            // AutoNumber: ST_TextAutonumberScheme ("arabicPeriod", "romanUcParenR"…)
  int startAt = 1;               // AutoNumber
  std::wstring font;             // empty: the first run's font (buFontTx)
  std::optional<draw::Color> color;  // empty: the first run's color (buClrTx)
  float sizePercent = 1;         // of the first run's size (buSzPct); with sizePoints 0
  float sizePoints = 0;          // > 0: buSzPts
  std::string pictureRel;        // Picture: a:buBlip's r:embed...
  std::string picturePart;       // ...in this part's relationships (the part whose style set it)
};

struct ParagraphProps {
  int level = 0;  // 0-8
  TextAlign align = TextAlign::Left;
  FontAlign fontAlign = FontAlign::Auto;
  float marginLeft = 0, marginRight = 0, indent = 0;  // points; indent is the first line's, from marginLeft
  Spacing lineSpacing{true, 1};
  Spacing spaceBefore, spaceAfter;
  Bullet bullet;
  std::vector<TabStop> tabs;
  float defaultTab = 72;  // points
  bool rtl = false;
  bool eastAsianLineBreak = true, latinLineBreak = false, hangingPunctuation = false;
};

enum class Underline : uint8_t {
  None, Single, Double, Heavy, Dotted, DottedHeavy, Dash, DashHeavy, DashLong, DashLongHeavy, DotDash, DotDashHeavy,
  DotDotDash, DotDotDashHeavy, Wavy, WavyHeavy, WavyDouble, Words
};
enum class Strike : uint8_t { None, Single, Double };
enum class Caps : uint8_t { None, Small, All };

struct RunProps {
  float size = 18;  // points
  bool bold = false, italic = false;
  Underline underline = Underline::None;
  Strike strike = Strike::None;
  Caps caps = Caps::None;
  float baseline = 0;  // fraction of the size: 0.3 is superscript, -0.25 subscript
  float spacing = 0;   // extra space between characters, points
  float kern = 0;      // kerning applies from this size up, points; 0: never
  // Typefaces, with the theme's (+mj-lt, +mn-ea…) resolved. Empty: none named.
  std::wstring latin, eastAsian, complexScript, symbol;
  // The text's fill, resolved. A gradient or pattern fill gives its first color here, and its
  // element in `fill` (resolve it against the context colors of the slide shown).
  draw::Color color{0, 0, 0, 1};
  bool noFill = false;
  pugi::xml_node fill;      // gradFill, pattFill or blipFill; null for solid and none
  pugi::xml_node outline;   // a:ln, null when none
  pugi::xml_node effects;   // a:effectLst (shadow, glow), null when none
  std::optional<draw::Color> highlight;
  std::optional<draw::Color> underlineColor;  // empty: the text's color (uFillTx)
  std::string lang;
  Hyperlink click, hover;
};

struct TextRun {
  enum class Kind : uint8_t { Text, Break, Field };
  Kind kind = Kind::Text;
  std::wstring text;  // Text, Field (a field's text resolved where the model can: slide numbers)
  RunProps props;
  std::string fieldType;  // Field: "slidenum", "datetime1"…; its text is what the file stored,
                          // except slide numbers, which are the slide's
  pugi::xml_node node;    // a:r, a:br or a:fld
};

struct Paragraph {
  ParagraphProps props;
  std::vector<TextRun> runs;
  RunProps endProps;  // a:endParaRPr over the list style: an empty paragraph's size
  int number = 0;     // AutoNumber bullets: this paragraph's number in its list
  pugi::xml_node node;  // a:p
};

// a:bodyPr, merged along the placeholder chain.
struct BodyProps {
  enum class Anchor : uint8_t { Top, Middle, Bottom, Justified, Distributed };
  enum class Vertical : uint8_t { Horizontal, Vertical, Vertical270, WordArtVertical, EastAsian, Mongolian, WordArtRtl };
  enum class Autofit : uint8_t { None, Shrink, ResizeShape };
  float left = 7.2f, top = 3.6f, right = 7.2f, bottom = 3.6f;  // insets, points
  Anchor anchor = Anchor::Top;
  bool anchorCenter = false;
  bool wrap = true;
  Vertical vertical = Vertical::Horizontal;
  float rotation = 0;  // degrees
  bool upright = false;
  bool rtlColumns = false;
  int columns = 1;
  float columnSpacing = 0;  // points
  Autofit autofit = Autofit::None;
  float fontScale = 1, lineSpacingReduction = 0;  // Shrink: what PowerPoint stored
  std::string warp;  // a:prstTxWarp prst ("textArchUp"…); empty: none
};

// A text body with everything resolved.
struct TextBody {
  BodyProps body;
  std::vector<Paragraph> paragraphs;
  pugi::xml_node node;      // the txBody as written
  bool Empty() const;       // no text at all (an empty placeholder: drawn without text)
};

// A date and time field's text now (type "datetime1".."datetime13"), in language `lang` (empty:
// the user's), as slides show it. Empty for other types. (P2: the header and footer dialog.)
std::wstring DateFieldText(std::string_view type, const std::string& lang);

}  // namespace fw::slide

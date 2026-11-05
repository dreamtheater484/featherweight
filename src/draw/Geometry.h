#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Painter.h"

namespace draw {

// Shape geometry as DrawingML defines it (ECMA-376 Part 1, §20.1.9): guides computed from the
// shape's size and its adjust values, and paths drawn with them. The preset shapes (§20.1.10.56,
// generated from the standard into PresetShapes.inc) and custom geometry (a:custGeom) share this
// model and its evaluator; VML's common shape types map onto the presets.
class Geometry {
 public:
  // How a path is filled: with the shape's fill, not at all, or lighter or darker than it (the
  // shading on shapes such as the cube and the can).
  enum class PathFill : uint8_t { Normal, None, Lighten, LightenLess, Darken, DarkenLess };

  struct Figure {
    Path path;  // in the shape's own coordinates: (0, 0) to its width and height, in points
    PathFill fill = PathFill::Normal;
    bool stroke = true;
  };
  struct Outline {
    std::vector<Figure> figures;
    Rect textRect;  // where text goes, before the text body's insets
  };
  // An adjust value set on a shape (a:avLst), replacing the default of the same name. Values are
  // as written: most are 1/100000 of the shape's size, angles are 1/60000 of a degree.
  struct Adjust {
    std::string name;
    double value = 0;
  };

  // A preset ("rect", "roundRect", "star5"…), or null when there's no such preset. Each preset is
  // read on first use and kept.
  static std::shared_ptr<const Geometry> Preset(std::string_view name);
  // Every preset's name, sorted.
  static std::vector<std::string_view> PresetNames();

  // Building custom geometry, in the file's order. Formulas and operands are as written:
  // "*/ w adj 100000", "wd2", "21600". Unknown names count as 0; untrusted input is capped.
  void AddAdjust(std::string_view name, std::string_view formula);
  void AddGuide(std::string_view name, std::string_view formula);
  // A new path with its own coordinate size (0: the shape's), fill and stroke.
  void BeginPath(double width, double height, PathFill fill, bool stroke);
  void MoveTo(std::string_view x, std::string_view y);
  void LineTo(std::string_view x, std::string_view y);
  void ArcTo(std::string_view wR, std::string_view hR, std::string_view stAng, std::string_view swAng);
  void QuadTo(std::string_view x1, std::string_view y1, std::string_view x2, std::string_view y2);
  void CubicTo(std::string_view x1, std::string_view y1, std::string_view x2, std::string_view y2,
               std::string_view x3, std::string_view y3);
  void Close();
  void SetTextRect(std::string_view l, std::string_view t, std::string_view r, std::string_view b);
  bool Empty() const { return paths_.empty(); }

  // The paths at `size` (points), with `adjust` replacing default adjust values.
  Outline Evaluate(Size size, const std::vector<Adjust>& adjust = {}) const;

 private:
  enum class Op : uint8_t { Val, MulDiv, AddSub, AddDiv, IfElse, Abs, At2, Cat2, Cos, Max, Min, Mod, Pin, Sat2, Sin,
                            Sqrt, Tan };
  enum class Cmd : uint8_t { Move, Line, Arc, Quad, Cubic, Close };
  struct Operand {
    double constant = 0;
    int var = -1;  // a variable's index, or -1 for the constant
  };
  struct Formula {
    Op op = Op::Val;
    Operand a, b, c;
    int var = 0;           // where its value goes
    bool adjust = false;   // an adjust value: a shape's avLst can replace it by name
  };
  struct Command {
    Cmd cmd;
    Operand args[6];
  };
  struct PathDef {
    double width = 0, height = 0;
    PathFill fill = PathFill::Normal;
    bool stroke = true;
    std::vector<Command> commands;
  };

  Operand ParseOperand(std::string_view token) const;
  Formula ParseFormula(std::string_view text) const;
  void Define(std::string_view name, std::string_view formula, bool adjust);
  void Add(Cmd cmd, std::initializer_list<std::string_view> args);
  void ReadCompact(std::string_view def);

  std::vector<std::string> names_;  // variables after the built-in ones, in definition order
  std::vector<Formula> formulas_;
  std::vector<PathDef> paths_;
  Operand rect_[4];
  bool hasRect_ = false;
  size_t commands_ = 0;
};

}  // namespace draw

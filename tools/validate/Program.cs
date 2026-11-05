// fwvalidate: does saving add validator errors? (plan §3: "Every saved file must have no new
// errors in the Open XML SDK validator.") Test tool only, never shipped.
//
//   fwvalidate --saved <folder> <original file or folder>...
//       Each file in the saved folder (fwtool roundtrip --out writes them, named like the original)
//       is validated, as is the original of the same name. An error the original doesn't have is a
//       new error, and fails the run.
//   fwvalidate --compare [--json FILE] (<original> <saved>... | --pairs FILE)
//       The same for explicit pairs: --pairs reads one "original<TAB>saved" per line (fwtool
//       torture's pairs.tsv, whose saved copies aren't named like their originals). --json writes
//       each pair's new errors, with where they are.
//   fwvalidate [--json FILE] <file>...
//       Lists each file's validator errors.
//
// Errors are compared by id, part and description, counting repeats, not by path: the model may
// write namespace prefixes differently, and an edit moves what follows it.
using System.Text.Json;
using DocumentFormat.OpenXml;
using DocumentFormat.OpenXml.Packaging;
using DocumentFormat.OpenXml.Validation;

record Problem(string Id, string Part, string Path, string Description) {
  public string Key => $"{Id} in {Part}: {Description}";
}

static class Program {
  static readonly string[] Known = { ".docx", ".docm", ".dotx", ".dotm", ".xlsx", ".xlsm", ".xltx", ".xltm",
                                     ".pptx", ".pptm", ".potx", ".potm", ".ppsx", ".ppsm" };
  static readonly OpenXmlValidator Validator = new(FileFormatVersions.Microsoft365) { MaxNumberOfErrors = 100000 };

  static int Main(string[] args) {
    string? saved = null, json = null;
    bool compare = false;
    var inputs = new List<string>();
    for (int i = 0; i < args.Length; i++) {
      if (args[i] == "--saved" && i + 1 < args.Length) saved = args[++i];
      else if (args[i] == "--compare") compare = true;
      else if (args[i] == "--json" && i + 1 < args.Length) json = args[++i];
      else if (args[i] == "--pairs" && i + 1 < args.Length) {
        compare = true;
        foreach (string line in File.ReadAllLines(args[++i])) {
          string[] cols = line.Split('\t');
          if (cols.Length == 2) inputs.AddRange(cols);
        }
      } else inputs.Add(args[i]);
    }
    if (inputs.Count == 0 || (compare && inputs.Count % 2 != 0)) {
      Console.Error.WriteLine("Usage: fwvalidate --saved <folder> <original file or folder>...\n" +
                              "       fwvalidate --compare [--json FILE] (<original> <saved>... | --pairs FILE)\n" +
                              "       fwvalidate [--json FILE] <file>...");
      return 2;
    }
    if (saved != null) return SavedFolder(saved, inputs);
    return compare ? Pairs(inputs, json) : List(inputs, json);
  }

  // --saved: the saved folder's files against the originals of the same name.
  static int SavedFolder(string saved, List<string> inputs) {
    var originals = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    foreach (string input in inputs) {
      IEnumerable<string> found = Directory.Exists(input)
          ? Directory.EnumerateFiles(input, "*", SearchOption.AllDirectories)
          : new[] { input };
      foreach (string f in found)
        if (Known.Contains(Path.GetExtension(f).ToLowerInvariant())) originals[Path.GetFileName(f)] = f;
    }
    int files = 0, failed = 0, originalErrors = 0;
    foreach (string copy in Directory.EnumerateFiles(saved).OrderBy(f => f, StringComparer.OrdinalIgnoreCase)) {
      if (!Known.Contains(Path.GetExtension(copy).ToLowerInvariant())) continue;
      string name = Path.GetFileName(copy);
      if (!originals.TryGetValue(name, out string? original)) {
        Console.WriteLine($"[skip] {name}: no original of that name");
        continue;
      }
      files++;
      var before = Validate(original, out string? beforeProblem);
      var after = Validate(copy, out string? afterProblem);
      originalErrors += before.Count;
      var added = NewProblems(before, after).GroupBy(p => p.Key).Select(g => $"{g.Count()} × {g.Key}").ToList();
      if (afterProblem != null && beforeProblem == null) added.Add("the saved copy can't be opened: " + afterProblem);
      if (added.Count == 0) {
        Console.WriteLine($"[ ok ] {name}: {after.Count} errors, as the original" +
                          (beforeProblem != null ? $" (the original can't be opened either: {beforeProblem})" : ""));
      } else {
        failed++;
        Console.WriteLine($"[FAIL] {name}: {added.Count} new");
        foreach (string a in added.Take(20)) Console.WriteLine("    " + a);
      }
    }
    Console.WriteLine();
    Console.WriteLine($"{files} files, {failed} with new validator errors ({originalErrors} errors in the originals)");
    return failed == 0 && files > 0 ? 0 : 1;
  }

  // --compare: explicit pairs, original then saved.
  static int Pairs(List<string> files, string? json) {
    var report = new List<object>();
    int failed = 0;
    for (int i = 0; i < files.Count; i += 2) {
      var before = Validate(files[i], out string? beforeProblem);
      var after = Validate(files[i + 1], out string? afterProblem);
      var added = NewProblems(before, after);
      if (afterProblem != null && beforeProblem == null) added.Add(Unopenable(afterProblem));
      Print($"{files[i + 1]} (new, against {Path.GetFileName(files[i])})", added);
      if (added.Count > 0) ++failed;
      report.Add(new { original = files[i], saved = files[i + 1], newErrors = added });
    }
    if (json != null) WriteJson(json, report);
    Console.WriteLine($"\n{files.Count / 2} files, {failed} with new errors");
    return failed > 0 ? 1 : 0;
  }

  // No mode: each file's errors.
  static int List(List<string> files, string? json) {
    var report = new List<object>();
    foreach (string f in files) {
      var problems = Validate(f, out string? problem);
      if (problem != null) problems.Add(Unopenable(problem));
      Print(f, problems);
      report.Add(new { file = f, errors = problems });
    }
    if (json != null) WriteJson(json, report);
    Console.WriteLine($"\n{files.Count} files");
    return 0;
  }

  static List<Problem> Validate(string path, out string? problem) {
    var result = new List<Problem>();
    problem = null;
    try {
      using OpenXmlPackage package = Open(path);
      foreach (ValidationErrorInfo e in Validator.Validate(package))
        result.Add(new Problem(e.Id ?? e.ErrorType.ToString(), e.Part?.Uri.ToString() ?? "", e.Path?.XPath ?? "",
                               e.Description ?? ""));
    } catch (Exception ex) {
      problem = ex.GetType().Name + ": " + ex.Message;
    }
    return result;
  }

  static OpenXmlPackage Open(string path) {
    var settings = new OpenSettings { AutoSave = false };
    string ext = Path.GetExtension(path).ToLowerInvariant();
    if (ext.StartsWith(".x")) return SpreadsheetDocument.Open(path, false, settings);
    if (ext.StartsWith(".p")) return PresentationDocument.Open(path, false, settings);
    return WordprocessingDocument.Open(path, false, settings);
  }

  static Problem Unopenable(string problem) => new("Package", "", "", "the SDK can't open it: " + problem);

  // The problems in `after` beyond those in `before`, counting repeats: the same error twice in the
  // copy but once in the original is new.
  static List<Problem> NewProblems(List<Problem> before, List<Problem> after) {
    var counts = new Dictionary<string, int>();
    foreach (var p in before) counts[p.Key] = counts.GetValueOrDefault(p.Key) + 1;
    var added = new List<Problem>();
    foreach (var p in after) {
      if (counts.GetValueOrDefault(p.Key) > 0) counts[p.Key]--;
      else added.Add(p);
    }
    return added;
  }

  static void Print(string label, List<Problem> problems) {
    Console.WriteLine($"{(problems.Count == 0 ? "[ ok ]" : "[FAIL]")} {label}: {problems.Count} {(problems.Count == 1 ? "error" : "errors")}");
    foreach (var p in problems.Take(20)) Console.WriteLine($"    {p.Part} {p.Path}: {p.Description} ({p.Id})");
    if (problems.Count > 20) Console.WriteLine($"    ... and {problems.Count - 20} more");
  }

  static void WriteJson(string path, List<object> report) =>
      File.WriteAllText(path, JsonSerializer.Serialize(new { files = report }, new JsonSerializerOptions { WriteIndented = true }));
}

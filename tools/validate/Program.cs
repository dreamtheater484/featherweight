// fwvalidate: the Open XML SDK's validator, for the never-damage tests (plan §3, §10). Not shipped.
//
//   fwvalidate [--json FILE] <file>...
//       Lists each file's validator errors (schema and semantic, against Microsoft 365's formats).
//   fwvalidate --compare [--json FILE] (<original> <saved>... | --pairs FILE)
//       Lists only the errors a saved file has that its original doesn't: the D2 gate is "no new
//       validator errors". Errors are compared by id, part and description, not by position, since
//       an edit moves what follows it. Fails (exit 1) when a saved file has new errors. --pairs
//       reads the pairs from a file, one "original<TAB>saved" per line (fwtool torture's pairs.tsv).
using System.Text.Json;
using DocumentFormat.OpenXml;
using DocumentFormat.OpenXml.Packaging;
using DocumentFormat.OpenXml.Validation;

record Problem(string Id, string Part, string Path, string Description) {
  public string Key => $"{Id}|{Part}|{Description}";
}

static class Program {
  static List<Problem> Validate(string file) {
    var problems = new List<Problem>();
    try {
      using OpenXmlPackage package = Path.GetExtension(file).ToLowerInvariant() switch {
        ".xlsx" or ".xlsm" or ".xltx" or ".xltm" => SpreadsheetDocument.Open(file, false),
        ".pptx" or ".pptm" or ".potx" or ".potm" or ".ppsx" or ".ppsm" => PresentationDocument.Open(file, false),
        _ => WordprocessingDocument.Open(file, false),
      };
      var validator = new OpenXmlValidator(FileFormatVersions.Microsoft365) { MaxNumberOfErrors = 10000 };
      foreach (ValidationErrorInfo e in validator.Validate(package))
        problems.Add(new Problem(e.Id ?? e.ErrorType.ToString(), e.Part?.Uri.ToString() ?? "", e.Path?.XPath ?? "", e.Description ?? ""));
    } catch (Exception ex) {
      problems.Add(new Problem("Package", "", "", $"the SDK can't open it: {ex.Message}"));
    }
    return problems;
  }

  // The problems in `saved` beyond those in `original`, counting repeats.
  static List<Problem> NewProblems(List<Problem> original, List<Problem> saved) {
    var counts = new Dictionary<string, int>();
    foreach (var p in original) counts[p.Key] = counts.GetValueOrDefault(p.Key) + 1;
    var added = new List<Problem>();
    foreach (var p in saved) {
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

  static int Main(string[] args) {
    bool compare = false;
    string? json = null;
    var files = new List<string>();
    for (int i = 0; i < args.Length; ++i) {
      if (args[i] == "--compare") compare = true;
      else if (args[i] == "--json" && i + 1 < args.Length) json = args[++i];
      else if (args[i] == "--pairs" && i + 1 < args.Length) {
        compare = true;
        foreach (var line in File.ReadAllLines(args[++i])) {
          var cols = line.Split('\t');
          if (cols.Length == 2) files.AddRange(cols);
        }
      } else files.Add(args[i]);
    }
    if (files.Count == 0 || (compare && files.Count % 2 != 0)) {
      Console.Error.WriteLine("Usage: fwvalidate [--json FILE] <file>...\n       fwvalidate --compare [--json FILE] (<original> <saved>... | --pairs FILE)");
      return 2;
    }
    var report = new List<object>();
    int failed = 0;
    if (compare) {
      for (int i = 0; i < files.Count; i += 2) {
        var added = NewProblems(Validate(files[i]), Validate(files[i + 1]));
        Print($"{files[i + 1]} (new, against {Path.GetFileName(files[i])})", added);
        if (added.Count > 0) ++failed;
        report.Add(new { original = files[i], saved = files[i + 1], newErrors = added });
      }
    } else {
      foreach (var f in files) {
        var problems = Validate(f);
        Print(f, problems);
        report.Add(new { file = f, errors = problems });
      }
    }
    if (json != null)
      File.WriteAllText(json, JsonSerializer.Serialize(new { files = report }, new JsonSerializerOptions { WriteIndented = true }));
    Console.WriteLine($"\n{(compare ? files.Count / 2 : files.Count)} files, {failed} with new errors");
    return compare && failed > 0 ? 1 : 0;
  }
}

// fwvalidate: does saving add validator errors? (plan §3: "Every saved file must have no new
// errors in the Open XML SDK validator.")
//
//   fwvalidate --saved <folder> <original file or folder>...
//
// Each file in the saved folder (fwtool roundtrip --out writes them, named like the original) is
// validated with the Open XML SDK, as is the original of the same name. An error the original
// doesn't have is a new error, and fails the run. Errors are compared by id, part and description,
// not by path: the model may write namespace prefixes differently.
using DocumentFormat.OpenXml;
using DocumentFormat.OpenXml.Packaging;
using DocumentFormat.OpenXml.Validation;

static class Program {
  static readonly string[] Known = { ".docx", ".docm", ".dotx", ".dotm", ".xlsx", ".xlsm", ".xltx", ".xltm",
                                     ".pptx", ".pptm", ".potx", ".potm", ".ppsx", ".ppsm" };

  static int Main(string[] args) {
    string? saved = null;
    var inputs = new List<string>();
    for (int i = 0; i < args.Length; i++) {
      if (args[i] == "--saved" && i + 1 < args.Length) saved = args[++i];
      else inputs.Add(args[i]);
    }
    if (saved == null || inputs.Count == 0) {
      Console.Error.WriteLine("Usage: fwvalidate --saved <folder> <original file or folder>...");
      return 2;
    }
    // The originals, by file name.
    var originals = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    foreach (string input in inputs) {
      IEnumerable<string> found = Directory.Exists(input)
          ? Directory.EnumerateFiles(input, "*", SearchOption.AllDirectories)
          : new[] { input };
      foreach (string f in found)
        if (Known.Contains(Path.GetExtension(f).ToLowerInvariant())) originals[Path.GetFileName(f)] = f;
    }
    var validator = new OpenXmlValidator(FileFormatVersions.Microsoft365) { MaxNumberOfErrors = 100000 };
    int files = 0, failed = 0, originalErrors = 0;
    foreach (string copy in Directory.EnumerateFiles(saved).OrderBy(f => f, StringComparer.OrdinalIgnoreCase)) {
      if (!Known.Contains(Path.GetExtension(copy).ToLowerInvariant())) continue;
      string name = Path.GetFileName(copy);
      if (!originals.TryGetValue(name, out string? original)) {
        Console.WriteLine($"[skip] {name}: no original of that name");
        continue;
      }
      files++;
      var before = Errors(validator, original, out string? beforeProblem);
      var after = Errors(validator, copy, out string? afterProblem);
      originalErrors += before.Values.Sum();
      // Counted per key: the same error twice in the copy but once in the original is new.
      var added = new List<string>();
      foreach (var (key, count) in after) {
        before.TryGetValue(key, out int had);
        if (count > had) added.Add($"{count - had} × {key}");
      }
      if (afterProblem != null && beforeProblem == null) added.Add("the saved copy can't be opened: " + afterProblem);
      if (added.Count == 0) {
        Console.WriteLine($"[ ok ] {name}: {after.Values.Sum()} errors, as the original" +
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

  static Dictionary<string, int> Errors(OpenXmlValidator validator, string path, out string? problem) {
    var result = new Dictionary<string, int>();
    problem = null;
    try {
      using OpenXmlPackage package = Open(path);
      foreach (ValidationErrorInfo e in validator.Validate(package)) {
        string key = $"{e.Id} in {e.Part?.Uri}: {e.Description}";
        result[key] = result.TryGetValue(key, out int n) ? n + 1 : 1;
      }
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
}

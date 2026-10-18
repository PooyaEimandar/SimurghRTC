// Exercise the validator in the actual DLL emitted by UBT, not a second implementation.
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;

if (args.Length != 4) throw new ArgumentException("Expected rules DLL, UBT directory, cases JSON, report JSON.");
AssemblyLoadContext.Default.Resolving += (_, name) => {
    string path = Path.Combine(args[1], name.Name + ".dll");
    return File.Exists(path) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(path) : null;
};
var method = Assembly.LoadFrom(args[0]).GetType("SimurghRTCMobileArtifact", throwOnError: true)
    .GetMethod("Validate", BindingFlags.Public | BindingFlags.Static);
using var document = JsonDocument.Parse(File.ReadAllText(args[2]));
var results = new List<object>();
int failures = 0;
foreach (var item in document.RootElement.EnumerateArray())
{
    bool accepted = false;
    string error = "";
    try
    {
        method.Invoke(null, new object[] { item.GetProperty("root").GetString(), item.GetProperty("platform").GetString() });
        accepted = true;
    }
    catch (TargetInvocationException exception) { error = exception.InnerException.Message; }
    bool passed = accepted == item.GetProperty("accept").GetBoolean();
    if (!passed) ++failures;
    results.Add(new { name = item.GetProperty("name").GetString(), passed, accepted, error });
}
File.WriteAllText(args[3], JsonSerializer.Serialize(new { failures, tests = results }, new JsonSerializerOptions { WriteIndented = true }));
Console.WriteLine($"Mobile rules: {results.Count - failures}/{results.Count} passed.");
return failures == 0 ? 0 : 1;

// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// Area banks contain different retail variants of some shared resources. Keep
// the already composed destination's variant, append missing dependencies, and
// report those differences. Explicitly requested effects must be byte-exact.
internal static class NativeEffectClosure {
    internal sealed record Result(byte[] Bytes,IReadOnlyList<string> SharedVariants);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    internal static Result Compose(byte[] destination,byte[] native,IReadOnlyCollection<int> required) {
        var target=BND4.Read(destination);var source=BND4.Read(native);
        var names=required.Select(id=>"f"+id.ToString("D9")+".fxr").ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach(string name in names)if(source.Files.Count(f=>Leaf(f.Name)==name)!=1)throw new InvalidDataException("Required native effect unavailable: "+name);
        if(target.Files.Select(f=>f.ID).Distinct().Count()!=target.Files.Count||target.Files.Select(f=>Leaf(f.Name)).Distinct(StringComparer.OrdinalIgnoreCase).Count()!=target.Files.Count||source.Files.Select(f=>Leaf(f.Name)).Distinct(StringComparer.OrdinalIgnoreCase).Count()!=source.Files.Count)throw new InvalidDataException("Duplicate native effect identity");
        int Category(BinderFile f)=>Leaf(f.Name).Split('.').Last().ToLowerInvariant() switch {"fxr"=>0,"tpf"=>100000,"flver"=>200000,"anibnd"=>300000,_=>throw new InvalidDataException("Unsupported native effect resource category")};
        var leaves=target.Files.ToDictionary(f=>Leaf(f.Name),StringComparer.OrdinalIgnoreCase);var ids=target.Files.Select(f=>f.ID).ToHashSet();var variants=new List<string>();
        foreach(var file in source.Files) {
            if(file.Bytes.Length==0)throw new InvalidDataException("Empty native effect dependency");string name=Leaf(file.Name);
            if(leaves.TryGetValue(name,out var previous)) {
                if(!previous.Bytes.Span.SequenceEqual(file.Bytes.Span)) {
                    if(names.Contains(name))throw new InvalidDataException("Required effect conflicts with prior owner: "+name);variants.Add(name);
                }
                continue;
            }
            int category=Category(file),id=category;while(ids.Contains(id))id++;if(id>=category+100000)throw new InvalidDataException("Native effect category exhausted");
            var copy=file.Clone();copy.ID=id;target.Files.Add(copy);leaves.Add(name,copy);ids.Add(id);
        }
        return new(target.Write(DCX.Type.DCX_DFLT_10000_44_9),variants);
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text.RegularExpressions;

// DS1 object materials can explicitly reference a sibling object's TPF. Resolve
// those authored references before creating a missing-texture fallback.
internal static class SourceObjectTextures {
    internal const int MaximumDepth=8,MaximumOwners=32;
    const int MaximumArchiveBytes=64*1024*1024;
    const long MaximumTotalBytes=256L*1024*1024;
    internal sealed record Dependency(string Owner,string Texture,string NextOwner);
    internal sealed record Result(IReadOnlyDictionary<string,TPF.Texture> Textures,IReadOnlyList<string> Owners,IReadOnlyList<Dependency> Dependencies,IReadOnlyList<string> Unresolved);
    sealed record Owner(IReadOnlyDictionary<string,TPF.Texture> Textures,IReadOnlyList<string> References);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Name(string p)=>Leaf(p).Split('.')[0];
    static string? ReferencedOwner(string path){var match=Regex.Match(path.Replace('\\','/'),@"(?:^|/)obj/(o\d{4})/tex/",RegexOptions.IgnoreCase|RegexOptions.CultureInvariant);return match.Success?match.Groups[1].Value.ToLowerInvariant():null;}
    static void Need(bool value,string why){if(!value)throw new InvalidDataException("Source object textures: "+why);}
    static bool Same(TPF.Texture a,TPF.Texture b)=>a.Format==b.Format&&a.Type==b.Type&&a.Mipmaps==b.Mipmaps&&a.Flags1==b.Flags1&&a.Bytes.AsSpan().SequenceEqual(b.Bytes);
    internal static Result Resolve(IEnumerable<string> explicitPaths,Func<string,byte[]?> read) {
        var owners=new Dictionary<string,Owner>(StringComparer.OrdinalIgnoreCase);var edges=new List<Dependency>();long total=0;
        Owner Load(string owner) {
            if(owners.TryGetValue(owner,out var cached))return cached;
            Need(owners.Count<MaximumOwners,"owner traversal budget");var bytes=read("obj/"+owner+".objbnd.dcx")??read("obj/"+owner+".objbnd");
            if(bytes==null){var absent=new Owner(new Dictionary<string,TPF.Texture>(),[]);owners.Add(owner,absent);return absent;}
            Need(bytes.Length is >0 and <=MaximumArchiveBytes,"compressed archive budget");var raw=DCX.Is(bytes)?DCX.Decompress(bytes).ToArray():bytes;Need(raw.Length<=MaximumArchiveBytes&&(total+=raw.Length)<=MaximumTotalBytes,"archive traversal byte budget");
            using BinderReader binder=BND3.Is(raw)?new BND3Reader(raw):BND4.Is(raw)?new BND4Reader(raw):throw new InvalidDataException("Source object textures: unsupported owner binder "+owner);
            Need(binder.Files.Count<=2048&&binder.Files.All(f=>f.UncompressedSize is >=0 and <=MaximumArchiveBytes)&&binder.Files.Sum(f=>f.UncompressedSize)<=MaximumArchiveBytes,"owner member budget");
            var textures=new Dictionary<string,TPF.Texture>(StringComparer.OrdinalIgnoreCase);var references=new List<string>();
            foreach(var f in binder.Files) {
                string leaf=Leaf(f.Name);bool tpf=leaf.EndsWith(".tpf",StringComparison.OrdinalIgnoreCase)||leaf.EndsWith(".tpf.dcx",StringComparison.OrdinalIgnoreCase);bool model=leaf.Equals(owner+".flver",StringComparison.OrdinalIgnoreCase)||leaf.Equals(owner+".flver.dcx",StringComparison.OrdinalIgnoreCase);
                if(!tpf&&!model)continue;var payload=binder.ReadFile(f);Need((total+=payload.Length)<=MaximumTotalBytes,"member traversal byte budget");
                if(tpf)foreach(var texture in TPF.Read(payload).Textures){string name=Name(texture.Name);if(textures.TryGetValue(name,out var old))Need(Same(old,texture),"ambiguous texture payload "+owner+"/"+name);else textures.Add(name,texture);}
                if(model){var data=DCX.Is(payload.ToArray())?DCX.Decompress(payload):payload;IFlver flver=FLVER2.Is(data.ToArray())?FLVER2.Read(data):FLVER0.Is(data.ToArray())?FLVER0.Read(data):throw new InvalidDataException("Source object textures: unsupported owner model "+owner);references.AddRange(flver.Materials.SelectMany(m=>m.Textures).Select(t=>t.Path).Where(p=>!string.IsNullOrEmpty(p)));}
            }
            var result=new Owner(textures,references);owners.Add(owner,result);return result;
        }
        TPF.Texture? Find(string owner,string name,HashSet<string> visiting,int depth) {
            Need(depth<MaximumDepth,"dependency depth budget");Need(visiting.Add(owner),"dependency cycle for "+name+" through "+owner);
            try {
                var data=Load(owner);if(data.Textures.TryGetValue(name,out var direct))return direct;TPF.Texture? found=null;
                foreach(string next in data.References.Where(p=>Name(p).Equals(name,StringComparison.OrdinalIgnoreCase)).Select(ReferencedOwner).OfType<string>().Where(next=>!next.Equals(owner,StringComparison.OrdinalIgnoreCase)).Distinct(StringComparer.OrdinalIgnoreCase)) {
                    edges.Add(new(owner,name,next));var texture=Find(next,name,visiting,depth+1);if(texture==null)continue;if(found!=null)Need(Same(found,texture),"ambiguous explicit dependency "+owner+"/"+name);else found=texture;
                }
                return found;
            } finally {visiting.Remove(owner);}
        }
        var output=new Dictionary<string,TPF.Texture>(StringComparer.OrdinalIgnoreCase);var unresolved=new List<string>();
        foreach(string path in explicitPaths.Where(p=>!string.IsNullOrEmpty(p)).Distinct(StringComparer.OrdinalIgnoreCase)) {
            string? owner=ReferencedOwner(path);if(owner==null)continue;string name=Name(path);var texture=Find(owner,name,new(StringComparer.OrdinalIgnoreCase),0);if(texture==null){unresolved.Add(path);continue;}if(output.TryGetValue(name,out var old))Need(Same(old,texture),"ambiguous requested texture "+name);else output.Add(name,texture);
        }
        return new(output,owners.Keys.ToArray(),edges.Distinct().ToArray(),unresolved);
    }
}

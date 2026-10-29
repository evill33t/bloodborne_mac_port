// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text.RegularExpressions;

internal static partial class Program {
    internal record StaticPlacement(string Name,string Model,Vector3 Position,Vector3 Rotation,Vector3 Scale);

    // Read-only, on-demand access to the two static-map layouts currently baked
    // into Bloodborne. Source Havok, scripts and dynamic object behavior stay local.
    sealed class StaticMapSource : IDisposable {
        readonly Assets assets;
        readonly string profile;
        readonly string map;
        readonly List<BinderReader> readers=[];
        readonly Dictionary<string,(BinderReader Reader,BinderFileHeader File)> textureFiles=new(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string,TPF.Texture> textureCache=new(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string,BinderFileHeader> mapFiles=new(StringComparer.OrdinalIgnoreCase);
        readonly BinderReader? mapModels;
        string? lastModel;
        Memory<byte>? lastData;
        Dictionary<string,TPF.Texture> lastLocal=new(StringComparer.OrdinalIgnoreCase);
        bool disposed;

        public IReadOnlyList<StaticPlacement> Placements { get; }
        public Vector3? PlayerStart { get; }

        public StaticMapSource(string root,string profile,string map) {
            if(profile is not("DS1" or "DS1R" or "DS2" or "DS2S"))throw new NotSupportedException("No static map source adapter for "+profile);
            if(!Regex.IsMatch(map,@"\Am\d\d_\d\d_\d\d_\d\d\z"))throw new ArgumentException("Invalid source map identifier");
            this.profile=profile;this.map=map;assets=new Assets(Canonical(root),profile);
            try {
                if(profile is "DS2" or "DS2S") {
                    var msb=MSB2.Read(assets.Read($"map/{map}/{map}.msb")??throw new FileNotFoundException("Source MSB unavailable"));
                    Placements=msb.Parts.GetEntries().Where(p=>p is MSB2.Part.MapPiece or MSB2.Part.Object)
                        .Select(p=>new StaticPlacement(p.Name,p.ModelName??"",p.Position,p.Rotation,p.Scale)).ToArray();
                    mapModels=assets.MapBinder(map)??throw new FileNotFoundException("Source model archive unavailable");readers.Add(mapModels);
                    foreach(var file in mapModels.Files.Where(f=>IsModel(f.Name)))mapFiles.TryAdd(ModelName(file.Name),file);
                    var textures=OpenSplit($"model/map/t{map[1..]}.tpfbhd")??throw new FileNotFoundException("Source texture archive unavailable");
                    IndexTextures(textures);
                } else {
                    var path=$"map/MapStudio/{map}.msb";
                    var msb=MSB1.Read(assets.Read(path)??assets.Read(path+".dcx")??throw new FileNotFoundException("Source MSB unavailable"));
                    PlayerStart=msb.Parts.Players.FirstOrDefault()?.Position;
                    Placements=msb.Parts.GetEntries().Where(p=>p is MSB1.Part.MapPiece or MSB1.Part.Object)
                        .Select(p=>new StaticPlacement(p.Name,p.ModelName??"",p.Position,p.Rotation,p.Scale)).ToArray();
                    var area=map[..3];
                    // DS1/Remastered map textures live in area-wide BXF3s, rather
                    // than in the individual block's model folder.
                    for(int i=0;i<4;++i)if(OpenSplit($"map/{area}/{area}_{i:D4}.tpfbhd") is {} textures)IndexTextures(textures);
                    if((assets.Read($"map/{area}/{area}_9999.tpf.dcx")??assets.Read($"map/{area}/{area}_9999.tpf")) is {} common) {
                        foreach(var texture in TPF.Read(common).Textures)textureCache.TryAdd(ModelName(texture.Name),texture);
                    }
                }
            } catch { Dispose();throw; }
        }

        static bool IsModel(string name)=>name.EndsWith(".flv",StringComparison.OrdinalIgnoreCase)||name.EndsWith(".flv.dcx",StringComparison.OrdinalIgnoreCase)
            ||name.EndsWith(".flver",StringComparison.OrdinalIgnoreCase)||name.EndsWith(".flver.dcx",StringComparison.OrdinalIgnoreCase);

        BinderReader? OpenSplit(string headerPath) {
            var header=assets.Read(headerPath);if(header==null)return null;
            var dataPath=headerPath[..^3]+"bdt";
            var loose=Path.Combine(assets.Root,dataPath);
            if(!File.Exists(loose))loose=Path.Combine(assets.Root,"dvdroot_ps4",dataPath);
            BinderReader reader;
            // Loose BDTs are mapped read-only; DS2's packed entry is materialized
            // only for this map. No whole-game extraction or source writes.
            if(header.AsSpan().StartsWith("BHF3"u8)) {
                reader=File.Exists(loose)?new BXF3Reader(header,loose):new BXF3Reader(header,assets.Read(dataPath)??throw new FileNotFoundException(dataPath));
            } else if(header.AsSpan().StartsWith("BHF4"u8)) {
                reader=File.Exists(loose)?new BXF4Reader(header,loose):new BXF4Reader(header,assets.Read(dataPath)??throw new FileNotFoundException(dataPath));
            } else throw new InvalidDataException("Unknown split source binder: "+headerPath);
            readers.Add(reader);return reader;
        }

        void IndexTextures(BinderReader reader) {
            foreach(var file in reader.Files)textureFiles.TryAdd(ModelName(file.Name),(reader,file));
        }

        public Memory<byte>? ReadModel(string model) {
            ObjectDisposedException.ThrowIf(disposed,this);
            if(!Regex.IsMatch(model,@"\A[mo][A-Za-z0-9_]+\z"))throw new ArgumentException("Invalid static source model identifier");
            if(lastModel==model)return lastData;
            lastModel=model;lastData=null;lastLocal=new(StringComparer.OrdinalIgnoreCase);
            if(mapModels!=null&&mapFiles.TryGetValue(model,out var file))lastData=ReadEntry(mapModels,file);
            if(model.StartsWith('o')) {
                var paths=profile is "DS2" or "DS2S"?new[]{$"model/obj/{model}.bnd",$"model/obj/{model}.texbnd"}
                    :new[]{$"obj/{model}.objbnd.dcx",$"obj/{model}.objbnd"};
                foreach(var path in paths)if(assets.Read(path) is {} bytes) {
                    var raw=Unpack(bytes);var files=BND4.Is(raw.ToArray())?BND4.Read(raw).Files:BND3.Is(raw.ToArray())?BND3.Read(raw).Files:throw new InvalidDataException("Unknown source object binder: "+path);
                    foreach(var entry in files) {
                        if(lastData==null&&IsModel(entry.Name))lastData=entry.Bytes;
                        if(entry.Name.EndsWith(".tpf",StringComparison.OrdinalIgnoreCase)||entry.Name.EndsWith(".tpf.dcx",StringComparison.OrdinalIgnoreCase))
                            foreach(var texture in TPF.Read(entry.Bytes).Textures)lastLocal.TryAdd(ModelName(texture.Name),texture);
                    }
                }
            }
            lastData??=FindModel(assets,profile,map,model);
            return lastData;
        }

        static Memory<byte> ReadEntry(BinderReader reader,BinderFileHeader file) {
            if(file.CompressedSize>512L*1024*1024||file.UncompressedSize>512L*1024*1024)throw new InvalidDataException("Source binder entry exceeds read budget: "+file.Name);
            return reader.ReadFile(file);
        }

        public IReadOnlyDictionary<string,TPF.Texture> TexturesForModel(string model) {
            var data=ReadModel(model);var result=new Dictionary<string,TPF.Texture>(lastLocal,StringComparer.OrdinalIgnoreCase);
            if(data==null)return result;
            var raw=Unpack(data.Value);IEnumerable<string> references;
            if(FLVER2.Is(raw.ToArray()))references=FLVER2.Read(raw).Materials.SelectMany(m=>m.Textures).Select(t=>t.Path);
            else if(FLVER0.Is(raw.ToArray()))references=FLVER0.Read(raw).Materials.SelectMany(m=>m.Textures).Select(t=>t.Path);
            else throw new InvalidDataException("No FLVER geometry in source model: "+model);
            foreach(var key in references.Where(p=>!string.IsNullOrEmpty(p)).Select(ModelName).Distinct(StringComparer.OrdinalIgnoreCase)) {
                if(result.ContainsKey(key))continue;
                if(!textureCache.ContainsKey(key)&&textureFiles.TryGetValue(key,out var entry))
                    foreach(var loaded in TPF.Read(ReadEntry(entry.Reader,entry.File)).Textures)textureCache.TryAdd(ModelName(loaded.Name),loaded);
                if(textureCache.TryGetValue(key,out var texture))result[key]=texture;
            }
            return result;
        }

        public void Dispose() {
            if(disposed)return;disposed=true;
            foreach(var reader in readers)reader.Dispose();readers.Clear();assets.Dispose();
        }
    }
}

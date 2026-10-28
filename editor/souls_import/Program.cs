// SPDX-License-Identifier: GPL-2.0-or-later
// Optional source adapter. SoulsFormats stays outside the native editor's document/runtime.
using SoulsFormats;
using Andre.Core;
using Andre.Formats.Util;
using System.Numerics;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Text.Json;

internal record Profile(string Id, string Name, string Reader);
internal record Placement(string Name, string Model, uint Kind, Vector3 Position, Vector3 Rotation, Vector3 Scale, int SourceId=-1);
internal record Mesh(Vector3[] Positions, int[] Indices, uint Color);
internal static partial class Program {
    static readonly Profile[] Profiles = [
        new("DES","Demon's Souls","MSBD"),new("DS1","Dark Souls","MSB1"),new("DS1R","Dark Souls Remastered","MSB1"),
        new("DS2","Dark Souls II","MSB2"),new("DS2S","Dark Souls II Scholar","MSB2"),new("DS3","Dark Souls III","MSB3"),
        new("BB","Bloodborne","MSBB"),new("SDT","Sekiro","MSBS"),new("ER","Elden Ring","MSBE"),
        new("AC6","Armored Core VI","MSB_AC6"),new("NR","Elden Ring Nightreign","MSB_NR"),
        new("DB_NT","Duskbloods Network Test","MSB_DB"),new("DB","Duskbloods","MSB_DB")];
    static readonly List<string> Notices=[];
    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static string ModelName(string name)=>Leaf(name).Split('.')[0];
    static Type? Reader(Profile p)=>typeof(MSBB).Assembly.GetType("SoulsFormats."+p.Reader);
    static int Main(string[] args) {
        try {
            if(args.Length==1 && args[0]=="self-test") {
                foreach(var p in Profiles.Where(p=>Reader(p)!=null)) {
                    var t=Reader(p)!;var original=(IMsb)Activator.CreateInstance(t)!;var bytes=original.Write(DCX.Type.None);
                    var read=t.GetMethods(BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy).Single(m=>m.Name=="Read"&&m.GetParameters().Length==1&&m.GetParameters()[0].ParameterType==typeof(Memory<byte>));
                    var reopened=(IMsb)read.Invoke(null,[new Memory<byte>(bytes)])!;
                    if(reopened.Parts.GetEntries().Count!=0)throw new InvalidDataException(p.Id+" empty fixture changed");Console.WriteLine(p.Id+" empty MSB fixture passed");
                }return 0;
            }
            if(args.Length==0 || args[0]=="profiles") {
                foreach(var p in Profiles)Console.WriteLine($"{p.Id}\t{p.Name}\t{(Reader(p)==null?"Reader unavailable in installed SoulsFormats":"Loose MSB adapter available")}");
                return 0;
            }
            var values=new Dictionary<string,string>();
            for(int i=1;i<args.Length;i+=2){if(i+1==args.Length)throw new ArgumentException("Missing value for "+args[i]);values.Add(args[i],args[i+1]);}
            string Get(string key)=>values.TryGetValue(key,out var v)?v:throw new ArgumentException("Missing "+key);
            var profile=Profiles.Single(p=>p.Id==Get("--profile"));var root=Canonical(Get("--root"));
            if(args[0]=="maps")return ListMaps(profile,root,Get("--smithbox"));
            if(args[0]=="character")return PreviewCharacter(values,profile,root);
            if(args[0]=="bake-skin")return BakeSkin(values,profile,root);
            if(args[0]!="import")throw new ArgumentException("Expected profiles, self-test, maps or import");
            var map=Get("--map");
            if(!Regex.IsMatch(map,@"\Am\d\d_\d\d_\d\d_\d\d\z"))throw new ArgumentException("Invalid map identifier");
            var smithbox=Get("--smithbox");
            var type=Reader(profile)??throw new NotSupportedException("Update Smithbox: installed SoulsFormats has no reader for "+profile.Name);
            using var assets=new Assets(root,profile.Id);
            var msbPath=profile.Id is "DS2" or "DS2S"?$"map/{map}/{map}.msb":$"map/MapStudio/{map}.msb.dcx";
            byte[] mapBytes=assets.Read(msbPath)??assets.Read(msbPath.Replace(".dcx",""))??throw new FileNotFoundException("Map unavailable. Use a loose game dump, or DS2's installed Game folder.",msbPath);
            // Static inherited Read(Memory<byte>) is part of SoulsFormats' generic file contract.
            var method=type.GetMethods(BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy).Single(m=>m.Name=="Read" && m.GetParameters().Length==1 && m.GetParameters()[0].ParameterType==typeof(Memory<byte>));
            var msb=(IMsb)method.Invoke(null,[new Memory<byte>(mapBytes)])!;
            var placements=msb.Parts.GetEntries().Select(p=>new Placement(p.Name,p.ModelName??"",Kind(p.GetType().Name),p.Position,p.Rotation,p.Scale)).ToList();
            if(profile.Id is "DS2" or "DS2S")ReadGenerators(assets,smithbox,profile.Id,map,msb,placements);
            if(values.GetValueOrDefault("--characters-only")=="1")placements=placements.Where(p=>p.Kind==2).ToList();
            var models=new Dictionary<string,Mesh>();
            using var packedMap=assets.MapBinder(map);
            long vertices=0,indices=0;
            foreach(var name in placements.Where(p=>p.Kind<=2).OrderBy(p=>p.Kind==2?0:p.Kind==1?1:2).Select(p=>p.Model).Where(n=>n.Length>0).Distinct()) {
                try {
                    var bytes=packedMap?.Files.FirstOrDefault(f=>ModelName(f.Name)==name && (f.Name.EndsWith(".flv.dcx")||f.Name.EndsWith(".flver.dcx")));
                    Memory<byte>? data=null;if(bytes!=null)data=packedMap!.ReadFile(bytes);
                    data??=FindModel(assets,profile.Id,map,name);
                    if(!data.HasValue || data.Value.IsEmpty)continue;
                    var mesh=Decode(data.Value,name);
                    if(mesh.Positions.Length==0||mesh.Indices.Length==0)throw new InvalidDataException("Model contains no drawable geometry");
                    if(vertices+mesh.Positions.Length>8000000 || indices+mesh.Indices.Length>24000000)throw new InvalidDataException("Source preview exceeds the geometry budget");
                    vertices+=mesh.Positions.Length;indices+=mesh.Indices.Length;models.Add(name,mesh);
                } catch(Exception e) {Notices.Add($"{name}: {e.GetBaseException().Message}");}
            }
            var missing=placements.Where(p=>p.Kind<=2&&!models.ContainsKey(p.Model)).Select(p=>p.Model).Distinct().Count();
            Notices.Insert(0,$"Untextured bind-pose preview (terrain welded at 35 cm, other models at 2 cm; sky and alpha cards omitted). {missing} models unavailable. Collision, navigation, animations, AI, items and event logic are not converted.");
            var output=Canonical(Get("--output"));
            // Never overwrite files, including source game files. An incomplete output is removed on failure.
            if(output.Equals(root,StringComparison.OrdinalIgnoreCase)||output.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("Output must be outside the source game folder");
            using(var file=new FileStream(output,FileMode.CreateNew,FileAccess.Write)) {
                try {using var w=new BinaryWriter(file,Encoding.UTF8,true);w.Write(Encoding.ASCII.GetBytes("BBMSRC01"));Text(w,profile.Id);Text(w,map);Text(w,string.Join('\n',Notices));
                    w.Write(models.Count);foreach(var (name,mesh) in models){Text(w,name);w.Write(mesh.Positions.Length);w.Write(mesh.Indices.Length);w.Write(mesh.Color);foreach(var p in mesh.Positions)Vector(w,p);foreach(var i in mesh.Indices)w.Write(i);}
                    w.Write(placements.Count);foreach(var p in placements){Text(w,p.Name);Text(w,p.Model);w.Write(p.Kind);w.Write(p.SourceId);Vector(w,p.Position);Vector(w,p.Rotation);Vector(w,p.Scale);}
                } catch {file.Dispose();File.Delete(output);throw;}
            }
            Console.WriteLine($"IMPORTED {profile.Id} {map}: {placements.Count} placements, {models.Count} models, {indices/3} model triangles -> {output}");
            foreach(var n in Notices)Console.WriteLine(n);
            return 0;
        } catch(Exception e) {Console.Error.WriteLine("Souls import: "+e.GetBaseException().Message);return 1;}
    }
    static int ListMaps(Profile profile,string root,string smithbox) {
        var type=Reader(profile)??throw new NotSupportedException("Update Smithbox: installed SoulsFormats has no reader for "+profile.Name);
        var names=new SortedDictionary<string,string>(StringComparer.Ordinal);
        var aliases=Path.Combine(smithbox,"src/Smithbox.Data/Assets/Aliases",profile.Id,"MapNames.json");
        if(File.Exists(aliases)){
            if(new FileInfo(aliases).Length>4*1024*1024)throw new InvalidDataException("Map alias catalog exceeds size limit");
            using var json=JsonDocument.Parse(File.ReadAllBytes(aliases));
            foreach(var entry in json.RootElement.EnumerateArray()){
                var id=entry.GetProperty("ID").GetString()??"";if(!Regex.IsMatch(id,@"\Am\d\d_\d\d_\d\d_\d\d\z"))continue;
                var label=entry.GetProperty("Name").GetString()??id;
                if(entry.TryGetProperty("Tags",out var tags) && tags.EnumerateArray().Any(t=>t.GetString()=="unused"))label+=" [unused]";
                names[id]=label.Replace('\t',' ').Replace('\r',' ').Replace('\n',' ');
            }
        }
        foreach(var prefix in new[]{root,Path.Combine(root,"dvdroot_ps4")}){
            var folder=Path.Combine(prefix,"map","MapStudio");
            if(Directory.Exists(folder))foreach(var file in Directory.EnumerateFiles(folder,"*.msb*")){
                var id=Path.GetFileName(file).Split('.')[0];if(Regex.IsMatch(id,@"\Am\d\d_\d\d_\d\d_\d\d\z"))names.TryAdd(id,id);
                if(names.Count>10000)throw new InvalidDataException("Too many source maps");
            }
            folder=Path.Combine(prefix,"map");if(profile.Id is "DS2" or "DS2S" && Directory.Exists(folder))foreach(var child in Directory.EnumerateDirectories(folder)){
                var id=Path.GetFileName(child);if(Regex.IsMatch(id,@"\Am\d\d_\d\d_\d\d_\d\d\z"))names.TryAdd(id,id);
            }
        }
        using var assets=new Assets(root,profile.Id);
        var method=type.GetMethods(BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy).Single(m=>m.Name=="Read" && m.GetParameters().Length==1 && m.GetParameters()[0].ParameterType==typeof(Memory<byte>));
        foreach(var (id,name) in names){
            var path=profile.Id is "DS2" or "DS2S"?$"map/{id}/{id}.msb":$"map/MapStudio/{id}.msb.dcx";
            var bytes=assets.Read(path)??assets.Read(path.Replace(".dcx",""));if(bytes==null)continue;
            var msb=(IMsb)method.Invoke(null,[new Memory<byte>(bytes)])!;
            Console.WriteLine($"{id}\t{name}\t{msb.Parts.GetEntries().Count}");
        }
        return 0;
    }
    static string Canonical(string path,int links=0) {
        if(links>40)throw new IOException("Too many source/output symlinks");
        var full=Path.GetFullPath(path);var prefix=Path.GetPathRoot(full)!;
        foreach(var part in full.Substring(prefix.Length).Split(Path.DirectorySeparatorChar,StringSplitOptions.RemoveEmptyEntries)) {
            prefix=Path.Combine(prefix,part);FileSystemInfo info=Directory.Exists(prefix)?new DirectoryInfo(prefix):new FileInfo(prefix);
            if(info.LinkTarget!=null){var target=info.ResolveLinkTarget(true)??throw new IOException("Unresolved source/output symlink");prefix=Canonical(target.FullName,links+1);}
        }return prefix;
    }
    static uint Kind(string type)=>type.Contains("Enemy")?2u:type.Contains("Object")||type=="Asset"?1u:type=="MapPiece"?0u:3u;
    static void Text(BinaryWriter w,string text){var b=Encoding.UTF8.GetBytes(text);if(b.Length>1048576)throw new InvalidDataException("Source metadata too large");w.Write(b.Length);w.Write(b);}
    static void Vector(BinaryWriter w,Vector3 p){if(!float.IsFinite(p.X)||!float.IsFinite(p.Y)||!float.IsFinite(p.Z))throw new InvalidDataException("Nonfinite source coordinates");w.Write(p.X);w.Write(p.Y);w.Write(p.Z);}
    static Memory<byte>? FindModel(Assets a,string profile,string map,string name) {
        // Only identifiers from known model classes become relative asset paths.
        if(!Regex.IsMatch(name,@"\A[mcoa][A-Za-z0-9_]+\z"))return null;
        if(profile is "BB" or "DS1" or "DS1R")map=map.Substring(0,6)+"_00_00";
        string[] paths=name[0] switch {
            'm'=>[$"map/{map}/{name}.flver.dcx",$"map/{map}/{map}_{name.Substring(1)}.flver.dcx",$"map/{map}/{name}.flver",$"map/{map}/{name}A{map.Substring(1,2)}.flver.dcx",$"map/{map}/{name}.mapbnd.dcx",$"map/{map}/{map}_{name.Substring(1)}.mapbnd.dcx",$"map/{map.Substring(0,3)}/{map}/{name}.mapbnd.dcx"],
            'c'=>[$"chr/{name}.chrbnd.dcx",$"chr/{name}.chrbnd",$"model/chr/{name}.bnd"],
            'o'=>[$"obj/{name}.objbnd.dcx",$"obj/{name}.objbnd",$"model/obj/{name}.bnd"],
            'a'=>[$"asset/aeg/{name.Substring(0,Math.Min(6,name.Length))}/{name}.geombnd.dcx",$"asset/environment/geometry/{name}.geombnd.dcx"],_=>[]};
        foreach(var path in paths)if(a.Read(path) is {} bytes) {
            if(path.Contains("bnd")) {
                var raw=Unpack(bytes);var files=BND4.Is(raw.ToArray())?BND4.Read(raw).Files:BND3.Is(raw.ToArray())?BND3.Read(raw).Files:throw new InvalidDataException("Unknown model binder");
                var entry=files.FirstOrDefault(f=>f.Name.EndsWith(".flver",StringComparison.OrdinalIgnoreCase)||f.Name.EndsWith(".flv",StringComparison.OrdinalIgnoreCase)||f.Name.EndsWith(".flver.dcx",StringComparison.OrdinalIgnoreCase));if(entry!=null)return entry.Bytes;continue;
            }
            return bytes;
        }
        return null;
    }
    static Memory<byte> Unpack(Memory<byte> bytes)=>DCX.Is(bytes.ToArray())?DCX.Decompress(bytes):bytes;
    static bool SkipMaterial(string mtd,string name,bool terrain)=>new[]{"sky","moon","sun_","_al","grass","bloody","cloud"}.Any(word=>mtd.Contains(word,StringComparison.OrdinalIgnoreCase))
        ||new[]{"sky","cloud","天球"}.Any(word=>name.Contains(word,StringComparison.OrdinalIgnoreCase)) || terrain&&mtd.Contains("nolight",StringComparison.OrdinalIgnoreCase);
    static Mesh Decode(Memory<byte> bytes,string model) {
        var terrain=model.StartsWith("m");var cell=terrain?.35f:.02f;
        var raw=Unpack(bytes);var points=new List<Vector3>();var indices=new List<int>();
        if(FLVER2.Is(raw.ToArray())) {
            var flver=FLVER2.Read(raw);var nodes=flver.Nodes;var worlds=new Dictionary<int,Matrix4x4>();
            Matrix4x4 World(int node,HashSet<int>? stack=null){if(node<0||node>=nodes.Count)return Matrix4x4.Identity;if(worlds.TryGetValue(node,out var w))return w;stack??=[];if(!stack.Add(node))throw new InvalidDataException("Cyclic source skeleton");w=nodes[node].ComputeLocalTransform()*World(nodes[node].ParentIndex,stack);stack.Remove(node);return worlds[node]=w;}
            foreach(var m in flver.Meshes) {
                if(m.MaterialIndex<0||m.MaterialIndex>=flver.Materials.Count)throw new InvalidDataException("Invalid source material reference");if(SkipMaterial(flver.Materials[m.MaterialIndex].MTD,flver.Materials[m.MaterialIndex].Name,terrain))continue;
                var start=points.Count;
                foreach(var v in m.Vertices){var p=v.Position;if(!m.UseBoneWeights && v.NormalW>=0 && v.NormalW<m.BoneIndices.Count)p=Vector3.Transform(p,World(m.BoneIndices[v.NormalW]));points.Add(p);}
                foreach(var f in m.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None))foreach(var i in f.Triangulate(m.Vertices.Count<65535)){if(i<0||i>=m.Vertices.Count)throw new InvalidDataException("Invalid source index");indices.Add(start+i);}
            }
        } else if(FLVER0.Is(raw.ToArray())) {
            var f=FLVER0.Read(raw);
            foreach(var m in f.Meshes){if(m.MaterialIndex>=f.Materials.Count)throw new InvalidDataException("Invalid source material reference");if(SkipMaterial(f.Materials[m.MaterialIndex].MTD,f.Materials[m.MaterialIndex].Name,terrain))continue;var start=points.Count;foreach(var v in m.Vertices){var p=v.Position;if(!m.UseBoneWeights && v.BoneIndices[0]>=0 && v.BoneIndices[0]<m.BoneIndices.Length){var b=m.BoneIndices[v.BoneIndices[0]];if(b>=0 && b<f.Nodes.Count)p=Vector3.Transform(p,f.Nodes[b].ComputeLocalTransform());}points.Add(p);}foreach(var i in m.Triangulate(f.Header.Version,false,true)){if(i<0||i>=m.Vertices.Count)throw new InvalidDataException("Invalid source index");indices.Add(start+i);}}
        } else throw new InvalidDataException("No FLVER geometry");
        // Preview-only welding removes UV/normal splits and sub-cell triangles. Layout
        // transforms stay exact; these meshes are never serialized into a destination game.
        var welded=new List<Vector3>();var faces=new List<int>();var corners=new Dictionary<(int,int,int),int>();var remap=new Dictionary<int,int>();var seen=new HashSet<(int,int,int)>();
        int Corner(int i){if(remap.TryGetValue(i,out var r))return r;var p=points[i];if(!float.IsFinite(p.X)||!float.IsFinite(p.Y)||!float.IsFinite(p.Z)||p.LengthSquared()>1e12f)throw new InvalidDataException("Invalid model coordinates");var key=((int)MathF.Round(p.X/cell),(int)MathF.Round(p.Y/cell),(int)MathF.Round(p.Z/cell));if(!corners.TryGetValue(key,out r)){r=welded.Count;corners.Add(key,r);welded.Add(p);}remap.Add(i,r);return r;}
        for(int i=0;i+2<indices.Count;i+=3){int a=Corner(indices[i]),b=Corner(indices[i+1]),c=Corner(indices[i+2]);if(a==b||b==c||c==a)continue;int x=Math.Min(a,Math.Min(b,c)),z=Math.Max(a,Math.Max(b,c)),y=a+b+c-x-z;if(!seen.Add((x,y,z)))continue;faces.Add(a);faces.Add(b);faces.Add(c);}
        return new(welded.ToArray(),faces.ToArray(),0xffb4aaa0u);
    }
    static void ReadGenerators(Assets assets,string smithbox,string profile,string map,IMsb msb,List<Placement> parts) {
        PARAM Read(string stem){var p=PARAM.Read(assets.Read($"param/{stem}_{map}.param")??throw new FileNotFoundException(stem));var def=Path.Combine(smithbox,"src/Smithbox.Data/Assets/PARAM",profile,"Defs",p.ParamType+".xml");var schema=PARAMDEF.XmlDeserialize(def);
            // Only read the stable prefix used by placement. Later generator bitfields differ
            // between releases and are neither interpreted nor written by this adapter.
            var last=stem=="generatorlocation"?"RotationZ":stem=="generatorparam"?"GeneratorRegistParam":"EnemyId";
            var end=schema.Fields.FindIndex(f=>f.InternalName==last)+1;if(end==0)throw new InvalidDataException("Missing DS2 placement field "+last);
            schema.Fields.RemoveRange(end,schema.Fields.Count-end);
            var expected=stem=="generatorlocation"?24:stem=="generatorparam"?12:4;
            if(schema.GetRowSize()!=expected || p.DetectedSize<expected)throw new InvalidDataException("Unexpected DS2 placement prefix size");
            p.ApplyParamdef(schema);return p;}
        object Cell(PARAM.Row row,string name)=>row.Cells.First(c=>c.Def.InternalName==name).Value;
        try {
            var locations=Read("generatorlocation");var generators=Read("generatorparam").Rows.ToDictionary(r=>r.ID);var registrations=Read("generatorregistparam").Rows.ToDictionary(r=>r.ID);
            var characterModels=new Dictionary<int,string>();
            try {
                var regulation=SFUtil.DecryptDS2Regulation(Path.Combine(assets.Root,"enc_regulation.bnd.dcx"));
                var enemyFile=regulation.Files.First(f=>Leaf(f.Name).Equals("EnemyParam.param",StringComparison.OrdinalIgnoreCase));var enemies=PARAM.Read(enemyFile.Bytes);
                var def=PARAMDEF.XmlDeserialize(Path.Combine(smithbox,"src/Smithbox.Data/Assets/PARAM",profile,"Defs",enemies.ParamType+".xml"));
                var end=def.Fields.FindIndex(f=>f.InternalName=="modelId")+1;if(end!=1||def.GetRowSize()<4)throw new InvalidDataException("Unexpected enemy model prefix");def.Fields.RemoveRange(1,def.Fields.Count-1);enemies.ApplyParamdef(def);
                foreach(var enemy in enemies.Rows){var model=Convert.ToInt32(Cell(enemy,"modelId"));if(model>=0&&model<=99999)characterModels.TryAdd(enemy.ID,$"c{model:D4}");}
            }catch(Exception e){Notices.Add("Enemy model lookup unavailable: "+e.Message);}
            var offset=msb.Events.GetEntries().OfType<MSB2.Event.MapOffset>().Select(e=>e.Translation).FirstOrDefault();
            foreach(var loc in locations.Rows) {
                var enemy=-1;var reg=-1;if(generators.TryGetValue(loc.ID,out var gen)){reg=Convert.ToInt32(Cell(gen,"GeneratorRegistParam"));if(registrations.TryGetValue(reg,out var registration))enemy=Convert.ToInt32(Cell(registration,"EnemyId"));}
                float F(string n)=>Convert.ToSingle(Cell(loc,n));
                parts.Add(new($"generator_{loc.ID}",characterModels.GetValueOrDefault(enemy,$"EnemyId:{enemy}"),2,new(F("PositionX")+offset.X,F("PositionY")+offset.Y,F("PositionZ")+offset.Z),new(F("RotationX"),F("RotationY"),F("RotationZ")),Vector3.One,enemy));
            }
            Notices.Add($"DS2 generators: {locations.Rows.Count} locations joined to registration/enemy IDs. Enemy IDs are source references, not Bloodborne NPC/model IDs.");
        } catch(Exception e) {throw new InvalidDataException("DS2 generator tables could not be joined: "+e.Message,e);}
    }
    sealed class Assets:IDisposable {
        readonly string root;readonly List<(Dictionary<ulong,BHD5.FileHeader> index,FileStream data)> archives=[];
        public string Root=>root;
        public Assets(string root,string profile){this.root=root;
            var names=profile switch {"DS2" or "DS2S"=>new[]{"GameDataEbl"},"DS3"=>new[]{"DLC2","DLC1","Data5","Data4","Data3","Data2","Data1"},_=>Array.Empty<string>()};
            try {foreach(var name in names) {
                var header=Path.Combine(root,name+".bhd");if(!File.Exists(header))continue;
                byte[] bytes=File.ReadAllBytes(header);if(!bytes.AsSpan().StartsWith("BHD5"u8)) {
                    using var decrypted=SoulsFormats.Util.CryptographyUtility.DecryptRsaParallel(header,ArchiveKeys.GetKey(header,Enum.Parse<Game>(profile)),4);bytes=decrypted.ToArray();
                }
                var game=Enum.Parse<Game>(profile).AsBhdGame()??throw new NotSupportedException("No archive reader for "+profile);
                var index=BHD5.Read(bytes,game).Buckets.SelectMany(b=>b).GroupBy(f=>f.FileNameHash).ToDictionary(g=>g.Key,g=>g.First());archives.Add((index,File.OpenRead(Path.Combine(root,name+".bdt"))));
            }}catch{Dispose();throw;}
        }
        public byte[]? Read(string path) {
            if(path.Contains("..")||Path.IsPathRooted(path))throw new IOException("Unsafe source path");
            var file=Path.Combine(root,path);if(!File.Exists(file))file=Path.Combine(root,"dvdroot_ps4",path);
            if(File.Exists(file)){var length=new FileInfo(file).Length;if(length>512L*1024*1024)throw new IOException("Source file exceeds 512 MiB read budget");return File.ReadAllBytes(file);}
            foreach(var (index,data) in archives)if(index.TryGetValue(SFUtil.FromPathHash("/"+path.ToLowerInvariant()),out var entry)) {
                if(entry.PaddedFileSize<0||entry.PaddedFileSize>512*1024*1024)throw new IOException("Source archive entry exceeds 512 MiB read budget");return entry.ReadFile(data);
            }
            return null;
        }
        public BXF4Reader? MapBinder(string map){var h=Read($"model/map/{map}.mapbhd");if(h==null)return null;return new(h,Read($"model/map/{map}.mapbdt")??throw new IOException("Missing map data archive"));}
        public void Dispose(){foreach(var a in archives)a.data.Dispose();archives.Clear();}
    }
}

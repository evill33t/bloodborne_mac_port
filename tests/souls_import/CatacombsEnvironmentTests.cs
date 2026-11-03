// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;using System.Numerics;using System.Security.Cryptography;using System.Buffers.Binary;
internal static class CatacombsEnvironmentTests {
    static void Check(bool ok,string message){if(!ok)throw new InvalidDataException(message);}
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Near(float a,float b)=>Check(Math.Abs(a-b)<.00001f,$"Wrong numeric value {a} != {b}");
    static void Main(string[] args) {
        if(args.Length!=3)throw new ArgumentException("DS1 root, BB dvdroot and Smithbox directory required");string source=args[0],native=args[1],defs=Path.Combine(args[2],"src/Smithbox.Data/Assets/PARAM/DS1R/Defs");
        var hashes=new Dictionary<string,string>();byte[]? Read(string root,string path){var file=Path.Combine(root,path);if(!File.Exists(file))return null;var bytes=File.ReadAllBytes(file);hashes[file]=Convert.ToHexString(SHA256.HashData(bytes));return bytes;}
        byte[]? Source(string p)=>Read(source,p);byte[]? Native(string p)=>Read(native,p);
        var map=MSB1.Read(Source("map/MapStudio/m13_00_00_00.msb")!);
        var bindings=map.Parts.GetEntries().Where(p=>p is MSB1.Part.MapPiece or MSB1.Part.Object or MSB1.Part.Collision or MSB1.Part.Enemy).Select(p=>new EnvironmentBinding(p.Name,p.LightID,p.FogID,p.ScatterID,p.ShadowID,p is MSB1.Part.Collision,p.ToneMapID,p.ToneCorrectID)).ToArray();
        var converted=MapEnvironment.Load(Source,Native,"DS1R","m13_00_00_00","m40_05_00_00",defs,bindings,nativeTemplateMap:"m24_01_00_00");
        var before=converted.Files.ToDictionary(f=>f.Path,f=>f.Bytes);CatacombsEnvironment.Apply(converted,Source,defs);
        foreach(var file in converted.Files.Where(f=>f.Path.EndsWith(".gparam.dcx"))) {
            var g=GPARAM.Read(file.Bytes);var old=GPARAM.Read(before[file.Path]);
            float Value(string key,int id)=>((GPARAM.FloatField)g.Params.Single(p=>p.Key=="FogParamEditor").Fields.Single(f=>f.Key==key)).Values.Single(v=>v.ID==id).Value;
            int inside=converted.Rows.Single(r=>r.Kind=="Fog"&&r.SourceID==0).NativeID!.Value,outside=converted.Rows.Single(r=>r.Kind=="Fog"&&r.SourceID==10).NativeID!.Value;
            Near(Value("Fog-DistOffset",inside),-10);Near(Value("Fog-Dist",inside),35);Near(Value("Fog-DistOffset",outside),0);Near(Value("Fog-Dist",outside),220);
            foreach(int id in new[]{inside,outside}) {float start=Value("Fog-DistOffset",id),distance=Value("Fog-Dist",id),strength=Value("Fog-fogAlpha",id);Near(strength*(1-MathF.Pow(2,-((start+distance)-start)/distance)),.5f);}
            foreach(var group in old.Params)for(int f=0;f<group.Fields.Count;f++)foreach(var v in group.Fields[f].Values) {
                if(group.Key=="FogParamEditor"&&v.ID>=30000&&group.Fields[f].Key is "Fog-DistOffset" or "Fog-Dist" or "Fog-fogAlpha" or "Fog-heightFogDensity")continue;
                var now=g.Params.Single(p=>p.Key==group.Key).Fields[f].Values.Single(x=>x.ID==v.ID);Check(Equals(v.Value,now.Value)&&v.TimeOfDay==now.TimeOfDay,"Unrelated native or imported value changed");
            }
        }
        var sourceMtd=BND3.Read(Source("mtd/Mtd.mtdbnd.dcx")!);var nativeMtd=BND4.Read(Native("mtd/allmaterialbnd.mtdbnd.dcx")!);
        MTD From(IReadOnlyList<BinderFile> files,string path)=>MTD.Read(files.Single(f=>Leaf(f.Name).Equals(Leaf(path),StringComparison.OrdinalIgnoreCase)).Bytes);
        var corpseMaterial=new FLVER2.Material("corpse","P_DullLeather[DSB].mtd",0);
        new SourceMapMaterials(_=>throw new Exception("Character MTD must not be looked up as map surface"),_=>throw new Exception("Unexpected native lookup")).Convert(corpseMaterial,new FLVER2.Material("corpse","M[ARSN].mtd",0));
        var materials=new SourceMapMaterials(p=>From(sourceMtd.Files,p),p=>From(nativeMtd.Files,p));int count=0;
        var nativeModel=FLVER2.Read(Native("map/m24_01_00_00/m24_01_00_00_004570.flver.dcx")!);int originalLists=nativeModel.GXLists.Count;
        var originalItems=nativeModel.GXLists.Select(g=>g.Select(i=>(i.ID,i.Unk04,Data:Convert.ToHexString(i.Data))).ToArray()).ToArray();
        var retailSky=FLVER2.Read(Native("map/m24_01_00_00/m24_01_00_00_900000.flver.dcx")!);
        Check(retailSky.GXLists.SelectMany(g=>g).Any(i=>i.ID=="GX20"&&i.Unk04==100&&i.Data.Length==12&&Math.Abs(BinaryPrimitives.ReadSingleLittleEndian(i.Data.AsSpan(0,4))-.05f)<.00001f),"Retail moving sky GX20 contract changed");
        foreach(string name in new[]{"m0160B0A13","m0530B0A13","m0580B0A13","m0630B0A13","m0650B0A13","m0660B0A13","m0720B0A13"}) {
            var model=FLVER2.Read(Source("map/m13_00_00_00/"+name+".flver.dcx")!);var m=model.Materials.Single(m=>SourceMapMaterials.Crawling(m.MTD));var motion=SourceMapMaterials.CrawlingMotion(From(sourceMtd.Files,m.MTD));
            Check(motion.DiffusePerSecond==new Vector2(0,-.05f)&&motion.BumpPerSecond==new Vector2(0,.02f)&&motion.DetailPerSecond==Vector2.Zero,"Source crawling texture rates changed");
            var target=new FLVER2.Material("crawl",@"N:\SPRJ\data\Material\mtd\map\M[ARSN]_e.mtd",0);target.GXIndex=nativeModel.Materials[0].GXIndex;materials.Convert(m,target);materials.ApplyCrawling(m,nativeModel,target);nativeModel.Materials.Add(target);
            Check(target.MTD.Contains("BBImport_m13_material_"),"Crawling edge material missing");
            Check(target.GXIndex==originalLists+count,"Crawling mesh did not get a private native GX list");
            foreach(var (id,expected) in new[]{("GX20",new Vector2(0,-.05f)),("GX26",new Vector2(0,.02f))}) {
                var item=nativeModel.GXLists[target.GXIndex].Single(g=>g.ID==id);Check(item.Unk04==100&&item.Data.Length==12,"Crawling scroll ABI changed");
                Near(BinaryPrimitives.ReadSingleLittleEndian(item.Data.AsSpan(0,4)),expected.X);Near(BinaryPrimitives.ReadSingleLittleEndian(item.Data.AsSpan(4,4)),expected.Y);
            }
            count++;
        }
        var roundtrip=FLVER2.Read(nativeModel.Write());
        for(int g=0;g<originalLists;g++)for(int i=0;i<originalItems[g].Length;i++)Check(originalItems[g][i]==(roundtrip.GXLists[g][i].ID,roundtrip.GXLists[g][i].Unk04,Convert.ToHexString(roundtrip.GXLists[g][i].Data)),"Crawling animation changed a shared donor GX list");
        for(int i=0;i<count;i++)Check(roundtrip.GXLists[originalLists+i].Single(g=>g.ID=="GX20").Data.AsSpan(0,8).SequenceEqual(nativeModel.GXLists[originalLists+i].Single(g=>g.ID=="GX20").Data.AsSpan(0,8)),"Native scroll rate changed in FLVER serialization");
        var stone=new FLVER2.Material("stone",@"N:\SPRJ\data\Material\mtd\map\M[ARSN].mtd",0);materials.Convert(new FLVER2.Material("source","M_4Stone[DSB].mtd",0),stone);
        var tint=(float[])materials.Definitions.Single(d=>stone.MTD.EndsWith(d.Name,StringComparison.Ordinal)).Definition.Params.Single(p=>p.Name=="g_DiffuseMapColor").Value;foreach(float v in tint)Near(v,.6f);
        foreach(var (file,hash) in hashes)Check(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file)))==hash,"Original source archive changed: "+file);
        Console.WriteLine($"PASS: Catacombs indoor/outdoor fog midpoint; authored stone .6 gain; {count} private native animated-edge GX lists; {hashes.Count} original files unchanged. Texture motion requires runtime validation.");
    }
}

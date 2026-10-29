// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

internal static class MapBreakablesTests {
    static void Check(bool value,string message) {if(!value)throw new Exception(message);}
    static void Reject(Action action,string message) {try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static List<BinderFile> Binder(string path) {var raw=DCX.Decompress(File.ReadAllBytes(path));return BND3.Is(raw.ToArray())?BND3.Read(raw).Files:BND4.Read(raw).Files;}
    static FLVER2 Visual(List<BinderFile> files,string name)=>FLVER2.Read(files.Single(f=>Leaf(f.Name)==name+".flver").Bytes);
    static FLVER2.Material Material(FLVER2 surface,IFlverMaterial source) {
        var material=surface.Materials[0];var copy=new FLVER2.Material(source.Name,material.MTD,0){GXIndex=material.GXIndex};
        foreach(var t in material.Textures)copy.Textures.Add(new(t.ParamName,t.Path,t.TilingScale,t.TilingTypeU,t.TilingTypeV,t.Unk14,t.Unk18,t.Unk1C));return copy;
    }
    static void RestPose(FLVER2 source,FLVER2 output,IReadOnlyDictionary<int,int> mapping,Vector3 physicsScale) {
        Check(source.Meshes.Count==output.Meshes.Count,"Imported source mesh was omitted");
        var sourceWorlds=SkinRetarget.World(source.Nodes);var nativeWorlds=SkinRetarget.World(output.Nodes);
        for(int mi=0;mi<source.Meshes.Count;++mi) {
            var a=source.Meshes[mi];var b=output.Meshes[mi];Check(a.Vertices.Count==b.Vertices.Count,"Source vertex count changed");
            Check(!b.UseBoneWeights&&b.BoneIndices.Count==0,"Native rigid selectors are not global");
            for(int vi=0;vi<a.Vertices.Count;++vi) {
                var input=a.Vertices[vi];var actual=b.Vertices[vi];int bone=a.BoneIndices[input.BoneIndices[0]];
                Check(actual.NormalW==mapping[bone],"Vertex bound to the wrong native fragment");
                var expected=Vector3.Transform(input.Position,sourceWorlds[bone]);
                var restored=Vector3.Transform(actual.Position,nativeWorlds[actual.NormalW])*physicsScale;
                Check(Vector3.Distance(restored,expected)<.00003f,"Retargeted fragment changed its rest-pose appearance");
                Check(Vector3.Distance(actual.UVs[0],input.UVs[0])<.0008f,"Source UV changed");
            }
            var expectedIndices=StaticMapGeometry.Faces(source,a).SelectMany(f=>f.Indices).ToArray();
            var actualIndices=b.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None).SelectMany(f=>f.Indices).ToArray();
            Check(expectedIndices.SequenceEqual(actualIndices),"Source fragment topology changed");
        }
    }
    static int Main(string[] args) {
        if(args.Length!=2)throw new ArgumentException("Pass the DS1 Remastered root and Bloodborne root");
        string source=Path.GetFullPath(args[0]),native=Path.Combine(Path.GetFullPath(args[1]),"dvdroot_ps4");
        var sourceMap=MSB1.Read(Path.Combine(source,"map/MapStudio/m10_02_00_00.msb"));
        var destination=MSBB.Read(Path.Combine(native,"map/MapStudio/m24_01_00_01.msb.dcx"));
        var placements=sourceMap.Parts.Objects.Select(p=>new MapBreakables.Placement(p.Name,p.ModelName,p.Position,p.Rotation,p.Scale)).ToArray();
        var surface=FLVER2.Read(Path.Combine(native,"map/m24_01_00_00/m24_01_00_00_004570.flver.dcx"));
        var nativeBundlePath=Path.Combine(native,"obj/o240035.objbnd.dcx");var nativeBundle=Binder(nativeBundlePath);
        var sourcePaths=new[]{nativeBundlePath,Path.Combine(native,"param/gameparam/gameparam.parambnd.dcx"),Path.Combine(source,"param/GameParam/GameParam.parambnd.dcx")}
            .Concat(new[]{"o1495","o1496","o1497"}.Select(m=>Path.Combine(source,"obj",m+".objbnd.dcx"))).ToArray();
        var hashes=sourcePaths.ToDictionary(p=>p,p=>SHA256.HashData(File.ReadAllBytes(p)));
        var beforeMap=destination.Write();var beforeSurface=surface.Write();var offset=new Vector3(-143.8f,20.07f,164.63f);
        var export=MapBreakables.BakeFirelink(source,"DS1R",native,destination,placements,offset,surface,m=>Material(surface,m));
        Check(export.Models.Count==3&&export.Objects.Count==25&&export.ConsumedNames.Count==25&&export.Files.Count==4,"Wrong Firelink urn export coverage");
        Check(destination.Write().SequenceEqual(beforeMap)&&surface.Write().SequenceEqual(beforeSurface),"Exporter mutated its caller inputs");
        foreach(var report in export.Reports) {
            var sourceFiles=Binder(Path.Combine(source,"obj",report.Source+".objbnd.dcx"));
            var packed=BND4.Read(export.Files["obj/"+report.Target+".objbnd.dcx"]);
            Check(report.NativeFragments==47&&report.Fragments.Count==report.SourceFragments,"Source pieces lack native bodies");
            Check(report.Fragments.Select(f=>f.Target).Distinct().Count()==report.SourceFragments,"Source fragments share a native body");
            foreach(var hkx in nativeBundle.Where(f=>f.Name.EndsWith(".hkx"))) {
                var copy=packed.Files.Single(f=>Leaf(f.Name)==Leaf(hkx.Name).Replace("o240035",report.Target));
                Check(copy.Bytes.Span.SequenceEqual(hkx.Bytes.Span),"Native physics/fixups were modified");
            }
            var intact=Visual(packed.Files,report.Target);var broken=Visual(packed.Files,report.Target+"_1");
            Check(broken.Nodes.Select(n=>n.Name).SequenceEqual(Visual(nativeBundle,"o240035_1").Nodes.Select(n=>n.Name)),"Native physics binding names changed");
            RestPose(Visual(sourceFiles,report.Source),intact,new Dictionary<int,int>{{0,0}},report.PhysicsScaleMultiplier);
            RestPose(Visual(sourceFiles,report.Source+"_1"),broken,report.Fragments.ToDictionary(f=>f.Source,f=>f.Target),report.PhysicsScaleMultiplier);
            var actor=export.Objects.First(p=>p.ModelName==report.Target);var sourcePart=placements.First(p=>p.Model==report.Source);
            Check(Vector3.Distance(actor.Position,sourcePart.Position+offset)<.00001f,"Urn placement shifted");
            Check(Vector3.Distance(actor.Scale,sourcePart.Scale*report.PhysicsScaleMultiplier)<.00001f,"Physics scale compensation is missing");
            var malformed=Visual(sourceFiles,report.Source+"_1");var mesh=malformed.Meshes[0];int first=mesh.Vertices[mesh.FaceSets[0].Indices[0]].BoneIndices[0];
            mesh.Vertices[mesh.FaceSets[0].Indices[0]].BoneIndices[0]=(first+1)%mesh.BoneIndices.Count;
            Reject(()=>MapBreakables.RetargetVisual(malformed,broken,surface,"DS1R",Vector3.One/report.PhysicsScaleMultiplier,
                report.Fragments.ToDictionary(f=>f.Source,f=>f.Target),m=>Material(surface,m)),"Cross-fragment triangles accepted");
        }
        var oldGame=BND4.Read(File.ReadAllBytes(Path.Combine(native,"param/gameparam/gameparam.parambnd.dcx")));
        var newGame=BND4.Read(export.Files["param/gameparam/gameparam.parambnd.dcx"]);
        foreach(var f in oldGame.Files.Where(f=>Leaf(f.Name)!="ObjectParam.param"))
            Check(newGame.Files.Single(n=>n.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Unrelated game parameter table changed");
        var oldRows=MapBreakables.ObjectRows(oldGame.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes,true);
        var newRows=MapBreakables.ObjectRows(newGame.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes,true);
        Check(newRows.Rows.Count==oldRows.Rows.Count+3,"ObjectParam rows were lost or duplicated");
        for(int i=0;i<oldRows.Rows.Count;++i)Check(newRows.Rows[i].ID==oldRows.Rows[i].ID&&newRows.Rows[i].Name==oldRows.Rows[i].Name&&
            MapBreakables.Payload(newRows.Rows[i]).SequenceEqual(MapBreakables.Payload(oldRows.Rows[i])),
            $"Existing ObjectParam row/order/payload changed at {i}: oldID={oldRows.Rows[i].ID}, newID={newRows.Rows[i].ID}, oldName={oldRows.Rows[i].Name??"<null>"}, newName={newRows.Rows[i].Name??"<null>"}, oldData={Convert.ToHexString(MapBreakables.Payload(oldRows.Rows[i]))}, newData={Convert.ToHexString(MapBreakables.Payload(newRows.Rows[i]))}");
        foreach(var model in export.Models) {
            var row=newRows.Rows.Single(r=>r.ID==int.Parse(model.Name[1..]));
            Check(MapBreakables.Payload(row).SequenceEqual(MapBreakables.Payload(oldRows.Rows.Single(r=>r.ID==240035))),"Native break behavior was not cloned exactly");
            destination.Models.Objects.Add(model);
        }
        destination.Parts.Objects.AddRange(export.Objects);var reread=MSBB.Read(destination.Write());
        Check(export.Objects.All(o=>reread.Parts.Objects.Any(p=>p.Name==o.Name&&p.ModelName==o.ModelName)),"MSB actor/model references did not survive serialization");
        foreach(var path in sourcePaths)Check(SHA256.HashData(File.ReadAllBytes(path)).SequenceEqual(hashes[path]),"Original game asset changed");
        Console.WriteLine("Map breakables passed: 25 urn actors, 3 native object/parameter bundles, rigid fragment/physics references and original assets verified. Gameplay remains unverified.");return 0;
    }
}

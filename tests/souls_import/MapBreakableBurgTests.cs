// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

internal static class MapBreakableBurgTests {
    static void Check(bool v,string message){if(!v)throw new Exception(message);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static FLVER2 Visual(BND3 bank,string model)=>FLVER2.Read(bank.Files.Single(f=>Leaf(f.Name)==model+".flver").Bytes);
    static FLVER2 Visual(BND4 bank,string model)=>FLVER2.Read(bank.Files.Single(f=>Leaf(f.Name)==model+".flver").Bytes);
    static int Bone(FLVER2.Mesh mesh,FLVER.Vertex v){int i=v.BoneIndices[0];return mesh.BoneIndices.Count==0?i:mesh.BoneIndices[i];}
    static Matrix4x4[] Worlds(FLVER2 model) {var result=new Matrix4x4[model.Nodes.Count];for(int i=0;i<result.Length;i++){var m=model.Nodes[i].ComputeLocalTransform();int p=model.Nodes[i].ParentIndex;while(p>=0){m*=model.Nodes[p].ComputeLocalTransform();p=model.Nodes[p].ParentIndex;}result[i]=m;}return result;}
    static FLVER2.Material Material(FLVER2 surface,IFlverMaterial source) {var m=surface.Materials[0];return new(source.Name,m.MTD,0){GXIndex=m.GXIndex,Textures=m.Textures.Select(t=>new FLVER2.Texture(t.ParamName,t.Path,t.TilingScale,t.TilingTypeU,t.TilingTypeV,t.Unk14,t.Unk18,t.Unk1C)).ToList()};}
    static void Geometry(FLVER2 source,FLVER2 output,Vector3 scale,IReadOnlyDictionary<int,int>? fragments) {
        var a=Worlds(source);var b=Worlds(output);Check(output.Meshes.Count==source.Meshes.Count,"Burg breakable lost source meshes");
        for(int i=0;i<source.Meshes.Count;i++) {var old=source.Meshes[i];var current=output.Meshes[i];Check(current.Vertices.Count==old.Vertices.Count&&!current.UseBoneWeights&&current.BoneIndices.Count==0,"Burg rigid selectors/vertex population changed");
            bool overflow=old.Vertices.SelectMany(v=>v.UVs).Any(v=>v.X<short.MinValue/2048f||v.X>short.MaxValue/2048f||v.Y<short.MinValue/2048f||v.Y>short.MaxValue/2048f);float uvScale=overflow?.5f:1f;
            for(int j=0;j<old.Vertices.Count;j++){var input=old.Vertices[j];var actual=current.Vertices[j];int bone=Bone(old,input);if(fragments!=null)Check(actual.NormalW==fragments[bone],"Source broken fragment selected wrong native body");var expected=Vector3.Transform(input.Position,a[bone]);var restored=Vector3.Transform(actual.Position,b[actual.NormalW])*scale;Check(Vector3.Distance(restored,expected)<.00015f,"Burg native object changed authored visual rest shape");Check(Vector3.Distance(actual.UVs[0],input.UVs[0]*uvScale)<.0008f,"Burg prop lost authored texture density");}
            Check(StaticMapGeometry.Faces(source,old).SelectMany(f=>f.Indices).SequenceEqual(current.FaceSets.Where(f=>f.Flags==0).SelectMany(f=>f.Indices)),"Burg prop topology changed");
        }
    }
    internal static void Run(string sourceRoot,string native,FLVER2 surface,MapBreakables.Export firelink) {
        var hashes=new Dictionary<string,byte[]>();byte[] Read(string p){var b=File.ReadAllBytes(p);hashes.TryAdd(p,SHA256.HashData(b));return b;}
        var source=MSB1.Read(Read(Path.Combine(sourceRoot,"map/MapStudio/m10_01_00_00.msb")));var before=source.Write();var destination=MSBB.Read(Read(Path.Combine(native,"map/MapStudio/m24_01_00_01.msb.dcx")));var originalMap=destination.Write();
        var placements=source.Parts.Objects.Select(p=>new MapBreakables.Placement(p.Name,p.ModelName,p.Position,p.Rotation,p.Scale)).ToArray();var candidates=MapBreakables.CandidateModels("DS1R","m10_01_00_00");Check(candidates.Count==36&&placements.Count(p=>candidates.Contains(p.Model))==407,"Actual Burg candidate inventory changed");
        var oldParam=BND4.Read(firelink.Files["param/gameparam/gameparam.parambnd.dcx"]);var sourceParam=BND3.Read(Read(Path.Combine(sourceRoot,"param/GameParam/GameParam.parambnd.dcx")));var sourceRows=MapBreakables.ObjectRows(sourceParam.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes,false);
        var offset=new Vector3(-143.8f,20.07f,164.63f);var export=MapBreakables.BakeFirelink(sourceRoot,"DS1R",native,destination,placements,offset,surface,m=>Material(surface,m),gameParamOverride:firelink.Files["param/gameparam/gameparam.parambnd.dcx"],sourceMap:"m10_01_00_00");
        Check(export.Objects.Count==351&&export.ConsumedNames.Count==351&&export.Reports.Count==29&&export.Models.Count==29,"Ordinary Burg breakable coverage incomplete");
        Check(export.Skipped.Count==56&&export.Skipped.Count(s=>source.Parts.Objects.Single(p=>p.Name==s.SourceName).EntityID>=0)==10&&export.Skipped.All(s=>!export.ConsumedNames.Contains(s.SourceName)),"Scripted/unmatched source props were silently replaced");
        Check(export.Skipped.Count(s=>s.Model=="o1155")==5&&export.Limits.Any(s=>s.Contains("collider")),"Explosive barrel/missing scripted collision limits were not explicit");
        Check(export.Models.All(m=>int.Parse(m.Name[1..])>=690103)&&firelink.Models.All(m=>!export.Models.Any(n=>n.Name==m.Name)),"Burg export reused existing Firelink reserved model IDs");
        var packedParam=BND4.Read(export.Files["param/gameparam/gameparam.parambnd.dcx"]);var oldRows=MapBreakables.ObjectRows(oldParam.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes,true);var rows=MapBreakables.ObjectRows(packedParam.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes,true);
        Check(rows.Rows.Count==oldRows.Rows.Count+29,"Burg parameter append lost earlier tables/rows");for(int i=0;i<oldRows.Rows.Count;i++)Check(rows.Rows[i].ID==oldRows.Rows[i].ID&&rows.Rows[i].Name==oldRows.Rows[i].Name&&MapBreakables.Payload(rows.Rows[i]).SequenceEqual(MapBreakables.Payload(oldRows.Rows[i])),"Existing native/Firelink row changed");foreach(var f in oldParam.Files.Where(f=>Leaf(f.Name)!="ObjectParam.param"))Check(packedParam.Files.Single(n=>n.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Unrelated game parameter table changed");
        int vertices=0,fragments=0;var donorCache=new Dictionary<string,BND4>();
        foreach(var report in export.Reports) {
            Check(MapBreakables.Family(report.Source).Donors.Contains(report.Donor)&&report.Family==MapBreakables.Family(report.Source).Family,"Bounds scoring crossed a verified prop family");
            if(report.Source=="o1242")Check(report.Donor is "o220507" or "o322020" or "o322030","Chair received a barrel/box collider");
            if(report.Family=="table")Check(report.Donor is "o220506" or "o240023" or "o240024" or "o240025" or "o240026","Table received a barrel/cargo collider");
            var original=BND3.Read(Read(Path.Combine(sourceRoot,"obj/"+report.Source+".objbnd.dcx")));if(!donorCache.TryGetValue(report.Donor,out var donor))donorCache.Add(report.Donor,donor=BND4.Read(Read(Path.Combine(native,"obj/"+report.Donor+".objbnd.dcx"))));var output=BND4.Read(export.Files["obj/"+report.Target+".objbnd.dcx"]);
            Check(report.Fragments.Count==report.SourceFragments&&report.Fragments.Select(f=>f.Target).Distinct().Count()==report.SourceFragments&&report.SourceFragments<=report.NativeFragments,"Burg source fragments lack separate native bodies");Check(output.Files.Count==donor.Files.Count,"Native object resource membership changed");
            foreach(var f in donor.Files){var copy=output.Files.Single(n=>n.ID==f.ID);Check(copy.Name==f.Name.Replace(report.Donor,report.Target,StringComparison.Ordinal),"Native object member path not renamed");if(!Leaf(f.Name).EndsWith(".flver"))Check(copy.Bytes.Span.SequenceEqual(f.Bytes.Span),"Native physics/animation/texture bytes changed");}
            var intact=Visual(output,report.Target);var broken=Visual(output,report.Target+"_1");Check(broken.Nodes.Select(n=>n.Name).SequenceEqual(Visual(donor,report.Donor+"_1").Nodes.Select(n=>n.Name)),"Native Havok/fragment binding names changed");Geometry(Visual(original,report.Source),intact,report.PhysicsScaleMultiplier,null);Geometry(Visual(original,report.Source+"_1"),broken,report.PhysicsScaleMultiplier,report.Fragments.ToDictionary(f=>f.Source,f=>f.Target));
            var payload=MapBreakables.Payload(rows.Rows.Single(r=>r.ID==int.Parse(report.Target[1..])));var donorPayload=MapBreakables.Payload(oldRows.Rows.Single(r=>r.ID==int.Parse(report.Donor[1..])));var sourcePayload=MapBreakables.Payload(sourceRows.Rows.Single(r=>r.ID==int.Parse(report.Source[1..])));for(int i=0;i<32;i++)Check(payload[i]==(i<4||i==9?sourcePayload[i]:donorPayload[i]),"Burg native payload changed outside source HP/defense/common hit flags");
            foreach(var actor in export.Objects.Where(o=>o.ModelName==report.Target)){var name=export.SourceActors.Single(p=>p.Value==actor.Name).Key;var p=source.Parts.Objects.Single(p=>p.Name==name);Check(actor.Position==p.Position+offset&&actor.Rotation==p.Rotation&&Vector3.Distance(actor.Scale,p.Scale*report.PhysicsScaleMultiplier)<.00001f&&actor.EntityID==-1&&actor.AnimIDs.All(a=>a==-1),"Burg actor transform/init/source event namespace changed");}
            vertices+=intact.Meshes.Sum(m=>m.Vertices.Count)+broken.Meshes.Sum(m=>m.Vertices.Count);fragments+=report.SourceFragments;
        }
        destination.Models.Objects.AddRange(export.Models);destination.Parts.Objects.AddRange(export.Objects);var reread=MSBB.Read(destination.Write());Check(export.Objects.All(o=>reread.Parts.Objects.Any(p=>p.Name==o.Name&&p.ModelName==o.ModelName)),"Burg model/actor references failed MSB roundtrip");
        Check(source.Write().SequenceEqual(before),"Burg source placements changed");foreach(var (path,sha) in hashes)Check(SHA256.HashData(File.ReadAllBytes(path)).SequenceEqual(sha),"Original source/native prop asset changed: "+path);
        string directory=Path.Combine(Environment.CurrentDirectory,"out/map-editor/fidelity-followup/props");Directory.CreateDirectory(directory);File.WriteAllText(Path.Combine(directory,"regression.json"),JsonSerializer.Serialize(new{status="PASS",actors=export.Objects.Count,models=export.Models.Count,vertices,fragments,originalAssets=hashes.Count,export.Skipped,export.Limits,reports=export.Reports},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS Burg breakables:{export.Objects.Count} ordinary actors/{export.Models.Count} source models/{fragments} independent source fragments;{export.Skipped.Count} scripted/unmatched props explicit; native physics and originals preserved");
    }
}

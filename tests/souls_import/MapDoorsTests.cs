// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using StudioCore.Utilities;

static class MapDoorsTests {
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    static void Reject(Action a,string why){try{a();}catch(InvalidDataException){return;}throw new Exception(why);}
    static string Leaf(string s)=>s.Replace('\\','/').Split('/').Last();
    static PARAM Raw(Memory<byte> b){var p=PARAM.Read(b);var d=new PARAMDEF{ParamType=p.ParamType,DataVersion=p.ParamdefDataVersion,Unicode=true};d.Fields.Add(new PARAMDEF.Field(d,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=checked((int)p.DetectedSize)});p.ApplyParamdef(d);return p;}
    static Matrix4x4 Track(SplineCompressedAnimation.TransformTrack t,float frame){
        var pos=t.HasSplinePosition?new(t.SplinePosition.GetValueX(frame)??t.StaticPosition.X,t.SplinePosition.GetValueY(frame)??t.StaticPosition.Y,t.SplinePosition.GetValueZ(frame)??t.StaticPosition.Z):t.StaticPosition;
        var scale=t.HasSplineScale?new(t.SplineScale.GetValueX(frame)??t.StaticScale.X,t.SplineScale.GetValueY(frame)??t.StaticScale.Y,t.SplineScale.GetValueZ(frame)??t.StaticScale.Z):t.StaticScale;
        return Matrix4x4.CreateScale(scale)*Matrix4x4.CreateFromQuaternion(t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.StaticRotation)*Matrix4x4.CreateTranslation(pos);
    }
    static void Main(string[] args){
        var hashes=new Dictionary<string,string>();string Hash(byte[] b)=>Convert.ToHexStringLower(SHA256.HashData(b));
        byte[]? Read(string root,string p){var path=Path.Combine(root,p);if(!File.Exists(path))return null;var b=File.ReadAllBytes(path);hashes[path]=Hash(b);return b;}
        byte[]? Source(string p)=>Read(args[0],p);byte[]? Native(string p)=>Read(args[1],p);
        var source=MSB1.Read(Source("map/MapStudio/m10_01_00_00.msb")!);var sourceBefore=source.Write();
        var native=MSBB.Read(Native("map/MapStudio/m24_02_00_00.msb.dcx")!);var map=new MSBB();
        var model=(MSBB.Model.Collision)native.Models.Collisions[0].DeepCopy();model.Name="h800000";map.Models.Collisions.Add(model);
        var slot=(MSBB.Part.Collision)native.Parts.Collisions[0].DeepCopy();slot.Name="h800000_0000";slot.ModelName=model.Name;map.Parts.Collisions.Add(slot);
        var offset=new Vector3(-143.8f,20.07f,164.63f);var tris=new[]{new MapCollision.Triangle(new(-1000,-1000,-1000),new(1000,-1000,-1000),new(0,-1000,1000))};var chunks=new[]{new MapCollisionBindings.Chunk("fixture source floor",tris,tris)};
        var script=new EMEVD(EMEVD.Game.Bloodborne);script.Events.Add(new(0));script.Events.Add(new(50));var sentinel=new EMEVD.Event(12415920);sentinel.Instructions.Add(new(1001,0,1f));script.Events.Add(sentinel);var initial=script.Write();
        var donor=BND4.Read(Native("obj/o241013.objbnd.dcx")!);var surface=FLVER2.Read(Native("map/m24_01_00_00/m24_01_00_00_004570.flver.dcx")!);
        FLVER2.Material Material(IFlverMaterial m){var b=surface.Materials[0];return new(m.Name,b.MTD,0){GXIndex=b.GXIndex,Textures=b.Textures.Select(t=>new FLVER2.Texture(t.ParamName,t.Path,t.TilingScale,t.TilingTypeU,t.TilingTypeV,t.Unk14,t.Unk18,t.Unk1C)).ToList()};}
        MapDoors.VerifyReservations(args[1]);var result=MapDoors.Apply(Source,Native,_=>null,source,map,offset,chunks,[slot],initial,"m10_01_00_00","m40_03_00_00",surface,Material);
        Check(result.Bindings.Count==2&&result.ConsumedNames.SetEquals(new[]{"o1305_01","o1305_02"})&&result.Files.Count==2,"Havel door export coverage");
        Check(source.Write().AsSpan().SequenceEqual(sourceBefore),"Source door MSB mutated");
        var reread=MSBB.Read(map.Write());Check(reread.Parts.MapPieces.Count==0&&reread.Parts.Objects.Count==2&&reread.Events.ObjActs.Count==2,"Doors exported as static/disappearing geometry");
        var exported=BND4.Read(result.Files["obj/o696000.objbnd.dcx"]);Check(exported.Files.Count==3,"Animated door object closure");
        var visual=FLVER2.Read(exported.Files.Single(f=>f.ID==200).Bytes);var sourceBank=BND3.Read(Source("obj/o1305.objbnd.dcx")!);var original=FLVER2.Read(sourceBank.Files.Single(f=>f.ID==200).Bytes);var sw=SkinRetarget.World(original.Nodes);var nw=SkinRetarget.World(visual.Nodes);
        float restError=0;for(int m=0;m<original.Meshes.Count;m++)for(int v=0;v<original.Meshes[m].Vertices.Count;v++){
            var a=original.Meshes[m].Vertices[v];int bone=SkinRetarget.Bone(original.Meshes[m],a.BoneIndices[0]);var b=visual.Meshes[m].Vertices[v];Check(b.NormalW==(bone==1?1:0),"Door arch/leaf are not independent rigid nodes");restError=Math.Max(restError,Vector3.Distance(Vector3.Transform(a.Position,sw[bone]),Vector3.Transform(b.Position,nw[b.NormalW])));
        }Check(restError<.00003f,"Door rest visual changed");
        var physics=exported.Files.Single(f=>f.ID==300).Bytes.ToArray();var p=new NativeAnimation.Pack(physics);const int vertices=720+64;var points=Enumerable.Range(0,8).Select(i=>new Vector3(p.Float(vertices+i*16),p.Float(vertices+i*16+4),p.Float(vertices+i*16+8))).ToArray();var box=result.Bindings[0].Box;
        Check(Vector3.Distance(points.Aggregate(Vector3.Min),box.Center-box.Half)<1e-6f&&Vector3.Distance(points.Aggregate(Vector3.Max),box.Center+box.Half)<1e-6f&&p.Float(720+20)==box.Radius,"Native moving collision box differs from source core/radius");
        var sourceAnims=BND3.Read(sourceBank.Files.Single(f=>f.ID==400).Bytes);var clips=BND4.Read(exported.Files.Single(f=>f.ID==400).Bytes);var rig=NativeAnimation.Skeleton(clips.Files.Single(f=>f.ID==1000000).Bytes.ToArray());float motionError=0;
        foreach(var (nativeID,sourceID) in new[]{(1,0),(2,1)}){
            var bytes=clips.Files.Single(f=>f.ID==1000000000+nativeID).Bytes.ToArray();var pack=new NativeAnimation.Pack(bytes);int anim=pack.Object("hkaSplineCompressedAnimation");var data=pack.Array(anim+152,1,100000);var decoded=SplineCompressedAnimation.ReadSplineCompressedAnimByteBlock(false,bytes.AsSpan(pack.Data+data.At,data.Count).ToArray(),2,1).Single();
            Check(pack.Int(anim+56)==91&&Math.Abs(pack.Float(anim+16)-3)<1e-5f,"Source door frame/timing lost");var sourceClip=SourceAnimation.Decode(sourceAnims.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),sourceAnims.Files.Single(f=>f.ID==sourceID).Bytes.ToArray());var reference=sourceClip.Bones[3];var sr=Matrix4x4.CreateScale(reference.Scale)*Matrix4x4.CreateFromQuaternion(reference.Rotation)*Matrix4x4.CreateTranslation(reference.Translation);Check(Matrix4x4.Invert(sr,out var inv),"Source door reference singular");
            foreach(int frame in new[]{0,15,45,75,90}){
                var expected=rig.Local[1]*inv*sourceClip.Sample(frame/30f)[3];var actual=Track(decoded[1],frame);var tip=new Vector3(1.4052f,1,0);motionError=Math.Max(motionError,Vector3.Distance(Vector3.Transform(tip,expected),Vector3.Transform(tip,actual)));
            }
        }Check(motionError<.002f,"Converted native door motion differs from source swing");
        var events=EMEVD.Read(result.Script);Check(events.Events.Single(e=>e.ID==12415920).Instructions.Single().ArgData.AsSpan().SequenceEqual(sentinel.Instructions.Single().ArgData),"Havel death event changed");
        var keys=events.Events.Single(e=>e.ID==12415940);var ownedKeys=keys.Instructions.Where(i=>i.Bank==3&&i.ID==4).ToArray();Check(ownedKeys.Select(i=>BitConverter.ToInt32(i.ArgData,4)).SequenceEqual(new[]{902019,902100})&&ownedKeys.All(i=>i.ArgData[0]==255&&i.ArgData[1]==3&&i.ArgData[8]==1),"Watchtower/Master Key OR inventory gate changed");
        foreach(var binding in result.Bindings){var actor=reread.Parts.Objects.Single(p=>p.EntityID==binding.Entity);var action=reread.Events.ObjActs.Single(a=>a.ObjActEntityID==binding.ActionEntity);Check(actor.Position==source.Parts.Objects.Single(p=>p.Name==binding.SourceName).Position+offset&&actor.CollisionName==slot.Name,"Door transform/residency differs");Check(actor.GparamConfigStruct.LightSetID==0&&actor.GparamConfigStruct.FogParamID==0,"Door retained native orphanage lighting/fog5");Check(action.ObjActPartName==actor.Name&&action.ObjActParamID==binding.ActionParam&&action.EventFlagID==binding.OpenFlag,"Persistent native object action is detached");var state=events.Events.Single(e=>e.ID==12415941+binding.Entity-4031700);Check(state.Instructions.Any(i=>i.Bank==2005&&i.ID==7&&BitConverter.ToInt32(i.ArgData,4)==1)&&state.Instructions.All(i=>i.Bank!=2005||i.ID!=3),"Door state disappears the object instead of restoring open motion");}
        var nativeGame=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx")!);var game=BND4.Read(result.Files["param/gameparam/gameparam.parambnd.dcx"]);
        Check(nativeGame.Files.Count==game.Files.Count,"Native parameter binder member count changed");
        foreach(var pair in nativeGame.Files.Zip(game.Files)){var f=pair.First;var g=pair.Second;Check(f.ID==g.ID&&f.Name==g.Name,"Native parameter binder identity/order changed");if(Leaf(f.Name)is not("ObjectParam.param" or "ObjActParam.param")){Check(g.Bytes.Span.SequenceEqual(f.Bytes.Span),"Unowned parameter table changed");continue;}var a=Raw(f.Bytes);var b=Raw(g.Bytes);var ids=a.Rows.Select(r=>r.ID).ToHashSet();var kept=b.Rows.Where(r=>ids.Contains(r.ID)).ToArray();Check(kept.Length==a.Rows.Count,"Original parameter row multiplicity changed");foreach(var rows in a.Rows.Zip(kept)){var r=rows.First;var rr=rows.Second;Check(r.ID==rr.ID&&rr.Name==r.Name&&((byte[])rr.Cells.Single().Value).SequenceEqual((byte[])r.Cells.Single().Value),"Original native parameter row/order changed");}}
        var beforeFailure=map.Write();Reject(()=>MapDoors.Apply(Source,Native,_=>null,source,map,offset,chunks,[slot],initial,"m10_01_00_00","m40_03_00_00",surface,Material),"Duplicate door ownership accepted");Check(map.Write().AsSpan().SequenceEqual(beforeFailure),"Failed door ownership check mutated map");
        foreach(var (path,sha)in hashes)Check(Hash(File.ReadAllBytes(path))==sha,"Original door/game archive mutated");
        string output="out/map-editor/doors-native/fixture";Directory.CreateDirectory(output);foreach(var (path,b)in result.Files){string full=Path.Combine(output,path);Directory.CreateDirectory(Path.GetDirectoryName(full)!);File.WriteAllBytes(full,b);}File.WriteAllBytes(Path.Combine(output,"m40_03_00_00.msb.dcx"),map.Write(DCX.Type.DCX_DFLT_10000_44_9));File.WriteAllBytes(Path.Combine(output,"m40_03_00_00.emevd.dcx"),result.Script);File.WriteAllText(Path.Combine(output,"doors.json"),JsonSerializer.Serialize(new{result.Bindings,result.Limits,hashes,restError,motionError},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS: two keyed Havel doors; source swing maximum error{motionError*1000:F3}mm, rest{restError*1000:F3}mm; exact moving leaf box, persistent object actions, OR key gate, preserved native rows and{hashes.Count}original archives. Export fixture only; gameplay pending.");
    }
}

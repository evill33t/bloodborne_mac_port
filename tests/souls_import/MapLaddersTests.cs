// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class MapLaddersTests {
    static void Check(bool v,string message){if(!v)throw new Exception(message);}
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    static string Leaf(string s)=>s.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] b)=>Convert.ToHexStringLower(SHA256.HashData(b));
    static byte[] Pack(params int[] n)=>n.SelectMany(BitConverter.GetBytes).ToArray();
    static PARAM Objects(Memory<byte> bytes) {
        var p=PARAM.Read(bytes);var d=new PARAMDEF{ParamType=p.ParamType,DataVersion=p.ParamdefDataVersion,Unicode=true};
        d.Fields.Add(new PARAMDEF.Field(d,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=32});p.ApplyParamdef(d);return p;
    }
    static PARAM Actions(Memory<byte> bytes) {
        var p=PARAM.Read(bytes);Check(p.ParamType=="ACTIONBUTTON_PARAM_ST"&&p.DetectedSize==72&&p.ParamdefDataVersion==1,"Native action schema");
        var d=new PARAMDEF{ParamType=p.ParamType,DataVersion=1,Unicode=true};d.Fields.Add(new PARAMDEF.Field(d,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=72});p.ApplyParamdef(d);return p;
    }
    static byte[] Payload(PARAM.Row r)=>(byte[])r.Cells.Single().Value;
    static Matrix4x4 Local(FLVER.Node n)=>Matrix4x4.CreateScale(n.Scale)*Matrix4x4.CreateRotationX(n.Rotation.X)*Matrix4x4.CreateRotationZ(n.Rotation.Z)*Matrix4x4.CreateRotationY(n.Rotation.Y)*Matrix4x4.CreateTranslation(n.Translation);
    static Matrix4x4 Parent(IReadOnlyList<FLVER.Node> nodes,int i) {var world=Matrix4x4.Identity;while(i>=0){world*=Local(nodes[i]);i=nodes[i].ParentIndex;}return world;}
    static Matrix4x4 Instance(Vector3 position,Vector3 rotation) {rotation*=MathF.PI/180;return Matrix4x4.CreateRotationX(rotation.X)*Matrix4x4.CreateRotationZ(rotation.Z)*Matrix4x4.CreateRotationY(rotation.Y)*Matrix4x4.CreateTranslation(position);}
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1];var hashes=new Dictionary<string,string>();
        byte[]? Read(string root,string path) {var full=Path.Combine(root,path);if(!File.Exists(full))return null;var b=File.ReadAllBytes(full);hashes.TryAdd(full,Hash(b));return b;}
        byte[]? Source(string path)=>Read(sourceRoot,path);byte[]? Native(string path)=>Read(nativeRoot,path);
        var source=MSB1.Read(Source("map/MapStudio/m10_01_00_00.msb")!);byte[] sourceBefore=source.Write();
        var offset=new Vector3(-143.8f,20.07f,164.63f);
        var collision=SourceCollision.Read("DS1R","m10_01_00_00",Source,offset);
        var chunks=MapCollisionBindings.Partition(collision,collision.Parts.Count(p=>p.Layer=="h"&&p.PlayerSolid));
        var nativeMap=MSBB.Read(Native("map/MapStudio/m25_00_00_00.msb.dcx")!);var map=new MSBB();var slots=new List<MSBB.Part.Collision>();
        for(int i=0;i<chunks.Count;++i) {
            string name="h"+(800000+i);var model=(MSBB.Model.Collision)nativeMap.Models.Collisions[0].DeepCopy();model.Name=name;map.Models.Collisions.Add(model);
            var slot=(MSBB.Part.Collision)nativeMap.Parts.Collisions[0].DeepCopy();slot.Name=name+"_0000";slot.ModelName=name;slot.EntityID=-1;slots.Add(slot);map.Parts.Collisions.Add(slot);
        }
        // Existing source-render records must remain intact; the adapter's
        // additional objects have no visible meshes or replacement HKX bodies.
        for(int i=0;i<13;++i) {var part=source.Parts.Objects.Single(p=>p.EntityID==1011140+i);string model="m"+(700000+i);
            map.Models.MapPieces.Add(new MSBB.Model.MapPiece(model));map.Parts.MapPieces.Add(new MSBB.Part.MapPiece{Name="source_visual_"+part.Name,ModelName=model,Position=part.Position+offset,Rotation=part.Rotation});}
        byte[] before=map.Write();var script=new EMEVD(EMEVD.Game.Bloodborne){StringData=System.Text.Encoding.Unicode.GetBytes("preserved string metadata\0")};
        var ctor=new EMEVD.Event(0);ctor.Instructions.Add(new(2003,2,Pack(52100100,1)));ctor.Parameters.Add(new EMEVD.Parameter(0,0,0,4));script.Events.Add(ctor);script.Events.Add(new(50));
        var retained=new EMEVD.Event(12435001);retained.Instructions.Add(new(2003,2,Pack(52100300,1)));script.Events.Add(retained);byte[] originalScript=script.Write();
        const string paramPath="param/gameparam/gameparam.parambnd.dcx";var game=BND4.Read(Native(paramPath)!);var objectFile=game.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");
        var originalRows=Objects(objectFile.Bytes);var existing=new PARAM.Row(originalRows.Rows.Single(r=>r.ID==256000)){ID=694999,Name="Existing composed object"};existing.Cells.Single().Value=Payload(existing).ToArray();Payload(existing)[0]^=1;originalRows.Rows.Add(existing);objectFile.Bytes=originalRows.Write();
        byte[] composed=game.Write();byte[]? Composed(string path)=>path==paramPath?composed:null;
        MapLadders.VerifyReservations(nativeRoot);
        var result=MapLadders.Apply(Source,Native,Composed,source,map,offset,chunks,slots,originalScript,"m10_01_00_00","m40_03_00_00");
        Check(result.Bindings.Count==12&&result.Skipped.Count==1&&result.Skipped[0].SourceName=="o1208"&&result.ConsumedNames.Count==0,"Fixed/kick-down ladder coverage");
        Check(result.Files.Count==13&&result.Files.Keys.All(p=>p.StartsWith("obj/")||p==paramPath),"Unexpected ladder geometry/collision output");
        var readback=MSBB.Read(map.Write());float positionError=0,directionError=0;var allMarkers=new List<object>();int unchangedMarkers=0,adjustedMarkers=0;
        foreach(var b in result.Bindings) {
            var part=source.Parts.Objects.Single(p=>p.EntityID==b.SourceEntity);var authored=BND3.Read(Source("obj/"+part.ModelName+".objbnd.dcx")!);
            var sv=FLVER2.Read(authored.Files.Single(f=>Leaf(f.Name)==part.ModelName+".flver").Bytes);
            var output=BND4.Read(result.Files["obj/"+b.Model+".objbnd.dcx"]);
            Check(output.Files.Count==1&&output.Files[0].Name.EndsWith(b.Model+".flver"),"Invisible ladder donor gained unsupported physics/visual resources");
            var nv=FLVER2.Read(output.Files[0].Bytes);Check(nv.Meshes.Count==0&&nv.Materials.Count==0&&nv.Nodes.Count==1&&nv.Dummies.Count==5,"Native invisible ladder topology");
            var actor=readback.Parts.Objects.Single(p=>p.EntityID==b.Entity);
            Check(actor.Position==part.Position+offset&&actor.Rotation==part.Rotation&&actor.Scale==Vector3.One&&actor.AnimIDs.All(i=>i==-1),"Ladder instance transform/animation initialization");
            Check(actor.CollisionName==slots[MapCollisionBindings.ObjectSlot(chunks,part.CollisionName,part.Position+offset)].Name,"Ladder residency floor binding");
            Check(actor.SibPath.Contains("m40_03_00_00")&&actor.DrawGroups.All(v=>v==uint.MaxValue)&&actor.BackreadGroups.All(v=>v==uint.MaxValue),"Ladder owned map path/residency");
            foreach(var a in sv.Dummies) {
                var n=nv.Dummies.Single(d=>d.ReferenceID==a.ReferenceID);var sw=Parent(sv.Nodes,a.ParentBoneIndex)*Instance(part.Position+offset,part.Rotation);
                var nw=Parent(nv.Nodes,n.ParentBoneIndex)*Instance(actor.Position,actor.Rotation);
                var sourceWorld=Vector3.Transform(a.Position,sw);var nativeWorld=Vector3.Transform(n.Position,nw);
                bool adjusted=part.EntityID==1011141&&a.ReferenceID==194;var expectedOffset=adjusted?new Vector3(0,-.1f,0):Vector3.Zero;
                float delta=Vector3.Distance(sourceWorld+expectedOffset,nativeWorld);positionError=Math.Max(positionError,delta);
                float angle=1-Vector3.Dot(Vector3.Normalize(Vector3.TransformNormal(a.Forward,sw)),Vector3.Normalize(Vector3.TransformNormal(n.Forward,nw)));directionError=Math.Max(directionError,Math.Abs(angle));
                Check(delta<.0001f&&Math.Abs(angle)<1e-6f,"Source ladder endpoint/facing differs after native serialization");
                if(adjusted) {
                    Check(Math.Abs(nativeWorld.Y-sourceWorld.Y+.1f)<1e-5f&&nativeWorld.X==sourceWorld.X&&nativeWorld.Z==sourceWorld.Z,"Taurus adjustment changed more than194 height");
                    Check(nativeWorld.Y-1<43.775f&&sourceWorld.Y-1>43.775f,"Native descent cylinder fails to include observed platform point");adjustedMarkers++;
                } else {Check(nativeWorld==sourceWorld,"Unadjusted world marker changed");unchangedMarkers++;}
                Check(n.ParentBoneIndex==0&&n.AttachBoneIndex==-1&&!n.FollowAttachBone&&n.UseUpwardVector,"Native ladder static dummy binding");
                allMarkers.Add(new{b.SourceName,a.ReferenceID,sourceWorld,nativeWorld,expectedOffset});
            }
            Check(b.DescendPromptOffset==(part.EntityID==1011141?-.1f:0),"Per-ladder descent adjustment reporting");
            if(part.EntityID==1011141) {
                var upper=nv.Dummies.Single(d=>d.ReferenceID==194);var lower=nv.Dummies.Single(d=>d.ReferenceID==193);
                float originalUpper=upper.Position.Y+.1f;
                Check((int)(2*MathF.Abs(upper.Position.Y-lower.Position.Y)-3)==14&&(int)(2*MathF.Abs(originalUpper-lower.Position.Y)-3)==14,"Native rung count changed");
                Check(upper.Position.Y==8.89821f-.1f&&nv.Dummies.Single(d=>d.ReferenceID==192).Position.Y==8.998209f,"Taurus prompt/animation endpoint identity");
            }
            Check(b.StartFlag==52100900+(part.EntityID-1011140)*2&&b.StopFlag==b.StartFlag+1&&b.Entity==4031140+part.EntityID-1011140&&b.Model=="o"+(695000+part.EntityID-1011140),"Stable ladder ownership mapping");
        }
        Check(unchangedMarkers==59&&adjustedMarkers==1,"Unexpected ladder marker adaptation coverage");
        var restored=MSBB.Read(map.Write());foreach(var b in result.Bindings){restored.Parts.Objects.RemoveAll(p=>p.Name==b.ObjectName);restored.Models.Objects.RemoveAll(m=>m.Name==b.Model);}
        Check(restored.Write().SequenceEqual(before),"Existing MSB/source visual/collision metadata changed");
        var emitted=EMEVD.Read(result.Script);Check(emitted.Format==EMEVD.Game.Bloodborne,"Emitted ladder script format");
        var registrations=emitted.Events.Single(e=>e.ID==0).Instructions.Where(i=>i.Bank==2009&&i.ID==0).ToArray();
        foreach(var b in result.Bindings)Check(registrations.Count(i=>i.Layer==null&&i.ArgData.SequenceEqual(Pack(b.StartFlag,b.StopFlag,b.Entity)))==1,"Native ladder registration arguments");
        Check(registrations.Length==12,"Unexpected registration count");emitted.Events.Single(e=>e.ID==0).Instructions.RemoveAll(i=>i.Bank==2009&&i.ID==0);
        Check(emitted.Write(DCX.Type.None).SequenceEqual(EMEVD.Read(originalScript).Write(DCX.Type.None)),"Existing EMEVD roots/events/parameters/strings changed");
        var outGame=BND4.Read(result.Files[paramPath]);var rows=Objects(outGame.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes);
        Check(rows.Rows.Count==originalRows.Rows.Count+12,"Unexpected object parameter count");
        foreach(var (row,i) in originalRows.Rows.Select((r,i)=>(r,i)))Check(rows.Rows[i].ID==row.ID&&rows.Rows[i].Name==row.Name&&Payload(rows.Rows[i]).SequenceEqual(Payload(row)),"Existing object parameter payload/order changed");
        foreach(var b in result.Bindings)Check(Payload(rows.Rows.Single(r=>r.ID==int.Parse(b.Model[1..]))).SequenceEqual(Payload(originalRows.Rows.Single(r=>r.ID==256000))),"Native ladder interaction/collision flags changed");
        foreach(var f in game.Files.Where(f=>Leaf(f.Name)!="ObjectParam.param"))Check(outGame.Files.Single(o=>o.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Unrelated parameter file changed");
        var fresh=MSBB.Read(before);var sourceCopy=MSB1.Read(sourceBefore);sourceCopy.Parts.Objects.Single(p=>p.EntityID==1011140).Scale=new Vector3(2);
        Reject(()=>MapLadders.Apply(Source,Native,Composed,sourceCopy,fresh,offset,chunks,slots,originalScript,"m10_01_00_00","m40_03_00_00"),"Scaled source ladder accepted");
        Reject(()=>MapLadders.Apply(Source,Native,Composed,source,fresh,offset,chunks,[],originalScript,"m10_01_00_00","m40_03_00_00"),"Unpaired collision slots accepted");
        var movedSource=MSB1.Read(sourceBefore);movedSource.Parts.Objects.Single(p=>p.EntityID==1011141).Position+=new Vector3(0,.01f,0);
        Reject(()=>MapLadders.Apply(Source,Native,Composed,movedSource,MSBB.Read(before),offset,chunks,slots,originalScript,"m10_01_00_00","m40_03_00_00"),"Changed Taurus source placement accepted");
        var changedSource=BND3.Read(Source("obj/o1209.objbnd.dcx")!);var sourceVisualFile=changedSource.Files.Single(f=>Leaf(f.Name)=="o1209.flver");var changedVisual=FLVER2.Read(sourceVisualFile.Bytes);
        var upperSource=changedVisual.Dummies.Single(d=>d.ReferenceID==194);upperSource.Position+=new Vector3(0,.01f,0);sourceVisualFile.Bytes=changedVisual.Write();byte[] changedBundle=changedSource.Write();
        Reject(()=>MapLadders.Apply(p=>p=="obj/o1209.objbnd.dcx"?changedBundle:Source(p),Native,Composed,source,MSBB.Read(before),offset,chunks,slots,originalScript,"m10_01_00_00","m40_03_00_00"),"Changed Taurus source model accepted");
        var changedNative=BND4.Read(Native(paramPath)!);var actionFile=changedNative.Files.Single(f=>Leaf(f.Name)=="ActionButtonParam.param");var actions=Actions(actionFile.Bytes);
        BitConverter.GetBytes(-.9f).CopyTo(Payload(actions.Rows.Single(r=>r.ID==5010)),32);actionFile.Bytes=actions.Write();byte[] changedNativeGame=changedNative.Write();
        Reject(()=>MapLadders.Apply(Source,p=>p==paramPath?changedNativeGame:Native(p),Composed,source,MSBB.Read(before),offset,chunks,slots,originalScript,"m10_01_00_00","m40_03_00_00"),"Changed native5010 contact-height contract accepted");
        Reject(()=>MapLadders.Apply(Source,Native,p=>p==paramPath?changedNativeGame:null,source,MSBB.Read(before),offset,chunks,slots,originalScript,"m10_01_00_00","m40_03_00_00"),"Changed composed5010 contact-height contract accepted");
        var corrupt=EMEVD.Read(Source("event/m10_01_00_00.emevd.dcx")!);corrupt.Events.Single(e=>e.ID==11010008).Instructions[3].ArgData[0]^=1;byte[] badScript=corrupt.Write();
        Reject(()=>MapLadders.Apply(p=>p=="event/m10_01_00_00.emevd.dcx"?badScript:Source(p),Native,Composed,source,fresh,offset,chunks,slots,originalScript,"m10_01_00_00","m40_03_00_00"),"Changed source registration semantics accepted");
        var occupied=EMEVD.Read(originalScript);occupied.Events.Single(e=>e.ID==0).Instructions.Add(new(2003,2,Pack(52100900,1)));
        Reject(()=>MapLadders.Apply(Source,Native,Composed,source,fresh,offset,chunks,slots,occupied.Write(),"m10_01_00_00","m40_03_00_00"),"Occupied ladder flag accepted");
        fresh.Models.Objects.Add(new MSBB.Model.Object("o695000"));byte[] occupiedBefore=fresh.Write();
        Reject(()=>MapLadders.Apply(Source,Native,Composed,source,fresh,offset,chunks,slots,originalScript,"m10_01_00_00","m40_03_00_00"),"Occupied ladder model accepted");
        Check(fresh.Write().SequenceEqual(occupiedBefore),"Rejected ladder stage partially mutated MSB");
        var firelink=MSB1.Read(Source("map/MapStudio/m10_02_00_00.msb")!);var empty=MapLadders.Apply(Source,_=>throw new Exception("Native reads for empty map"),_=>null,firelink,new MSBB(),offset,[],[],originalScript,"m10_02_00_00","m40_02_00_00");
        Check(empty.Files.Count==0&&empty.Script.SequenceEqual(originalScript)&&empty.Bindings.Count==0,"Firelink no-ladder stage changed content");
        Check(source.Write().SequenceEqual(sourceBefore),"Source MSB mutated");foreach(var (path,hash) in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"Installed asset changed: "+path);
        Directory.CreateDirectory("out/map-editor/ladders");File.WriteAllText("out/map-editor/ladders/regression.json",JsonSerializer.Serialize(new{status="PASS",bindings=result.Bindings,skipped=result.Skipped,positionError,directionError,unchangedMarkers,adjustedMarkers,allMarkers,sourceAndNativeAssetsPreserved=hashes.Count,result.Limits,result.NativeHashes},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"Ladders PASS:12 fixed,1 explicit kick-down limitation,59 unchanged world markers and1 checked10cm prompt adjustment; max residual position error {positionError*1000:F5}mm; {hashes.Count} source/native assets unchanged. Native descent pending.");
    }
}

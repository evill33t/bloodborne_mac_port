// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

static class BossFogWallsTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static string Leaf(string s)=>s.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] b)=>Convert.ToHexStringLower(SHA256.HashData(b));
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    static byte[] Payload(PARAM.Row row)=>(byte[])row.Cells.Single().Value;
    static void Main(string[] args) {
        string source=args[0],native=args[1];var hashes=new Dictionary<string,string>();
        byte[] Read(string root,string path) {string full=Path.Combine(root,path);var bytes=File.ReadAllBytes(full);hashes.TryAdd(full,Hash(bytes));return bytes;}
        byte[]? Native(string path)=>File.Exists(Path.Combine(native,path))?Read(native,path):null;
        var sourceMap=MSB1.Read(Read(source,"map/MapStudio/m10_01_00_00.msb"));
        var original=Native("map/MapStudio/m24_01_00_01.msb.dcx")!;var map=MSBB.Read(original);byte[] before=map.Write();
        var sourceParts=new[]{"o1405_0000","o1406_0000","o1413_0000"}.Select(n=>sourceMap.Parts.Objects.Single(p=>p.Name==n)).ToArray();
        var offset=new Vector3(-143.8f,20.07f,164.63f);var walls=new List<BossFogWalls.Wall>();var sourceBounds=new List<object>();
        for(int i=0;i<sourceParts.Length;++i) {
            var p=sourceParts[i];var bundle=BND3.Read(Read(source,"obj/"+p.ModelName+".objbnd.dcx"));
            var tag=new SourceCollision.Tag(bundle.Files.Single(f=>Leaf(f.Name)==p.ModelName+".hkx").Bytes.ToArray());
            var body=tag.Items.Single(t=>tag.Name(t.Type)=="hkpRigidBody");
            var shape=tag.Reference(tag.Nested(body.Type,body.At,"collidable","shape").At);
            Check(tag.Name(shape.Type)=="hkpConvexTranslateShape","Source fog wrapper changed");
            var shift=tag.Vector(tag.Nested(shape.Type,shape.At,"translation").At);
            var child=tag.Reference(tag.Nested(shape.Type,shape.At,"childShape","childShape").At);
            Check(tag.Name(child.Type)=="hkpBoxShape","Source fog is not a box");
            var half=tag.Vector(tag.Nested(child.Type,child.At,"halfExtents").At);
            float radius=BitConverter.Int32BitsToSingle(tag.Int(tag.Nested(child.Type,child.At,"radius").At));
            var bodyWorld=tag.Transform(tag.Nested(body.Type,body.At,"motion","motionState","transform").At);
            var noTranslation=bodyWorld;noTranslation.Translation=Vector3.Zero;
            Check(noTranslation==Matrix4x4.Identity&&radius==.025f,"Unsupported source fog body transform/radius");
            var center=Vector3.Transform(shift,bodyWorld);Check(Vector3.Distance(half,i<2?new(1.2782525f,1.975f,.025f):new(1.725f,2.725f,.025f))<1e-6f,"Source dimensions changed");
            walls.Add(new(p.Name,p.Position+offset,p.Rotation,p.Scale,4031800+i,4033800+i,map.Parts.Collisions[0].Name,half,center,radius,ReverseInteraction:i==0));
            sourceBounds.Add(new{p.Name,half,center,radius});
        }
        const string paramPath="param/gameparam/gameparam.parambnd.dcx",bankPath="sfx/frpg_sfxbnd_m40.ffxbnd.dcx";
        var nativeParams=BND4.Read(Native(paramPath)!);var stagedParams=BND4.Read(Native(paramPath)!);
        var stagedObject=stagedParams.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");var stagedRows=BossFogWalls.ObjectRows(stagedObject.Bytes);
        var marker=new PARAM.Row(stagedRows.Rows.Single(r=>r.ID==240308)){ID=693999,Name="Existing composed object"};
        marker.Cells.Single().Value=Payload(marker).ToArray();Payload(marker)[0]^=1;stagedRows.Rows.Add(marker);stagedObject.Bytes=stagedRows.Write();
        byte[] composedParams=stagedParams.Write(DCX.Type.DCX_DFLT_10000_44_9);
        var stagedBank=BND4.Read(Native("sfx/frpg_sfxbnd_m24.ffxbnd.dcx")!);
        stagedBank.Files.Add(new BinderFile(stagedBank.Files[0].Flags,99990,@"N:\SPRJ\data\INTERROOT_ps4\sfx\f999999990.fxr",new byte[]{1,2,3,4}));
        byte[] composedBank=stagedBank.Write(DCX.Type.DCX_DFLT_10000_44_9);
        byte[]? Composed(string path)=>path==paramPath?composedParams:path==bankPath?composedBank:null;
        var result=BossFogWalls.Apply(Native,Composed,map,walls);var readback=MSBB.Read(map.Write());
        Check(result.Bindings.Count==3&&result.Files.Count==5,"Fog output count");
        var restore=MSBB.Read(map.Write());foreach(var b in result.Bindings) {
            restore.Models.Objects.RemoveAll(m=>m.Name==b.Model);restore.Parts.Objects.RemoveAll(p=>p.Name==b.ObjectName);
            restore.Events.SFX.RemoveAll(e=>e.Name==b.SfxEventName);restore.Regions.Regions.RemoveAll(r=>r.Name==b.SfxRegionName);
        }
        Check(restore.Write().SequenceEqual(before),"Existing MSB content changed");
        var donorBundle=BND4.Read(Native("obj/o240308.objbnd.dcx")!);var donorHkx=donorBundle.Files.Single(f=>Leaf(f.Name)=="o240308.hkx").Bytes.ToArray();
        var donorObject=MSBB.Read(original).Parts.Objects.Single(p=>p.EntityID==2411810);
        float maxWorldError=0;
        foreach(var (b,i) in result.Bindings.Select((b,i)=>(b,i))) {
            var w=walls[i];var actor=readback.Parts.Objects.Single(p=>p.EntityID==w.ObjectEntity);
            Check(actor.ModelName==b.Model&&actor.Position==w.Position&&actor.Rotation==w.Rotation&&actor.Scale==Vector3.One&&actor.CollisionName==w.CollisionName,"Fog actor transform/reference");
            Check(actor.DrawGroups.All(g=>g==uint.MaxValue)&&actor.BackreadGroups.All(g=>g==uint.MaxValue)&&actor.AnimIDs.SequenceEqual(donorObject.AnimIDs)&&actor.BreakobjID.SequenceEqual(donorObject.BreakobjID),"Native fog initialization fields");
            var sfx=readback.Events.SFX.Single(e=>e.EntityID==w.SfxEntity);var origin=readback.Regions.GetEntries().Single(r=>r.Name==sfx.RegionName);
            Check(sfx.EffectID==824102&&!sfx.StartDisabled&&sfx.PartName==w.CollisionName&&origin.Shape is MSB.Shape.Point&&origin.Position==w.Position&&origin.Rotation==w.Rotation,"Native SFX binding");
            var bundle=BND4.Read(result.Files["obj/"+b.Model+".objbnd.dcx"]);var hkx=bundle.Files.Single(f=>Leaf(f.Name)==b.Model+".hkx").Bytes.ToArray();
            foreach(var f in donorBundle.Files.Where(f=>!f.Name.EndsWith(".hkx"))) {
                var actual=bundle.Files.Single(o=>o.Name==f.Name.Replace("o240308",b.Model,StringComparison.Ordinal));
                if(!w.ReverseInteraction||!f.Name.EndsWith(".flver"))Check(actual.Bytes.Span.SequenceEqual(f.Bytes.Span),"Native non-physics object asset changed");
                else {
                    var fv=FLVER2.Read(actual.Bytes);var donorVisual=FLVER2.Read(f.Bytes);var a=fv.Dummies.Single(d=>d.ReferenceID==191);var d=donorVisual.Dummies.Single(d=>d.ReferenceID==191);
                    Check(a.Position==d.Position&&a.Upward==d.Upward&&a.Forward==new Vector3(-d.Forward.X,d.Forward.Y,-d.Forward.Z),"Reverse interaction changed marker placement/up vector");
                    // This donor's first 64-byte dummy starts at0x80; only its
                    // Forward.X/Z sign bits may change in the serialized FLVER.
                    var expected=f.Bytes.ToArray();BitConverter.GetBytes(-d.Forward.X).CopyTo(expected,0x80+16);BitConverter.GetBytes(-d.Forward.Z).CopyTo(expected,0x80+24);
                    Check(actual.Bytes.Span.SequenceEqual(expected),"Reverse interaction changed fields outside dummy191 Forward.X/Z");
                    a.Forward=d.Forward;Check(fv.Write(DCX.Type.None).SequenceEqual(donorVisual.Write(DCX.Type.None)),"Reverse interaction changed other parsed FLVER fields");
                }
            }
            var prompt=FLVER2.Read(bundle.Files.Single(f=>Leaf(f.Name)==b.Model+".flver").Bytes).Dummies.Single(d=>d.ReferenceID==191);
            Check((prompt.Forward.Z<0)==w.ReverseInteraction&&b.ReverseInteraction==w.ReverseInteraction,"Fog prompt faces the wrong approach side");
            var (vertices,planes,radius,allowed)=Decode(hkx);
            Check(radius==w.Radius&&vertices.Count==8,"Source convex radius/vertices");
            var degrees=w.Rotation*(MathF.PI/180);var rotation=Matrix4x4.CreateRotationX(degrees.X)*Matrix4x4.CreateRotationZ(degrees.Z)*Matrix4x4.CreateRotationY(degrees.Y);
            for(int corner=0;corner<8;++corner) {
                var expected=w.Center+new Vector3((corner&1)==0?-w.HalfExtents.X:w.HalfExtents.X,(corner&2)==0?-w.HalfExtents.Y:w.HalfExtents.Y,(corner&4)==0?-w.HalfExtents.Z:w.HalfExtents.Z);
                var actual=vertices.MinBy(p=>Vector3.DistanceSquared(p,expected));Check(Vector3.Distance(actual,expected)<1e-6f,"Source box corner not retained");
                var sourceWorld=Vector3.Transform(expected,rotation)+sourceParts[i].Position+offset;var nativeWorld=Vector3.Transform(actual,rotation)+actor.Position;
                maxWorldError=Math.Max(maxWorldError,Vector3.Distance(sourceWorld,nativeWorld));
            }
            foreach(var p in vertices)foreach(var plane in planes)Check(Vector3.Dot(new(plane.X,plane.Y,plane.Z),p)+plane.W<2e-6f,"Convex planes exclude an emitted corner");
            for(int n=0;n<hkx.Length;++n)Check(allowed.Contains(n)||hkx[n]==donorHkx[n],"Unexpected native HKX byte mutation at "+n);
        }
        Check(maxWorldError<.00004f,"World-space source/native fog alignment");
        var outParams=BND4.Read(result.Files[paramPath]);var outRows=BossFogWalls.ObjectRows(outParams.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param").Bytes);
        Check(outRows.Rows.Count==stagedRows.Rows.Count+3,"Unexpected appended object rows");
        foreach(var (row,index) in stagedRows.Rows.Select((r,i)=>(r,i))) {var actual=outRows.Rows[index];Check(actual.ID==row.ID&&actual.Name==row.Name&&Payload(actual).SequenceEqual(Payload(row)),"Existing object parameter row changed");}
        foreach(var b in result.Bindings)Check(Payload(outRows.Rows.Single(r=>r.ID==int.Parse(b.Model[1..]))).SequenceEqual(Payload(stagedRows.Rows.Single(r=>r.ID==240308))),"Fog object physics/HP payload changed");
        foreach(var file in stagedParams.Files.Where(f=>Leaf(f.Name)!="ObjectParam.param"))Check(outParams.Files.Single(f=>f.ID==file.ID).Bytes.Span.SequenceEqual(file.Bytes.Span),"Unrelated parameter file changed");
        var bank=BND4.Read(result.Files[bankPath]);Check(bank.Files.Select(f=>f.ID).Distinct().Count()==bank.Files.Count,"Duplicate SFX binder IDs");
        foreach(var f in stagedBank.Files)Check(bank.Files.Single(o=>Leaf(o.Name)==Leaf(f.Name)).Bytes.Span.SequenceEqual(f.Bytes.Span)&&bank.Files.Single(o=>Leaf(o.Name)==Leaf(f.Name)).ID==f.ID,"Existing SFX resource changed");
        foreach(var f in BND4.Read(Native("sfx/frpg_sfxbnd_m24_01.ffxbnd.dcx")!).Files)Check(bank.Files.Single(o=>Leaf(o.Name)==Leaf(f.Name)).Bytes.Span.SequenceEqual(f.Bytes.Span),"Native fog dependency not retained");
        Check(bank.Files.All(f=>f.ID/100000==(f.Name.EndsWith(".fxr")?0:f.Name.EndsWith(".tpf")?1:2)),"SFX binder resource class changed");
        var clean=MSBB.Read(original);byte[] pristine=clean.Write();
        Reject(()=>BossFogWalls.Apply(Native,Composed,clean,[walls[0] with{Scale=new Vector3(2)}]),"Unsupported scaled fog accepted");
        Reject(()=>BossFogWalls.Apply(Native,Composed,clean,[walls[0] with{ObjectEntity=2411810}]),"Occupied entity accepted");
        Reject(()=>BossFogWalls.Apply(Native,Composed,clean,[walls[0],walls[0]]),"Duplicate source wall accepted");
        var damaged=donorHkx.ToArray();damaged[0]^=1;Reject(()=>BossFogWalls.FitBox(damaged,Vector3.One,Vector3.Zero,.025f),"Unsupported native box accepted");
        Reject(()=>BossFogWalls.FitBox(donorHkx,new(float.NaN,1,1),Vector3.Zero,.025f),"Nonfinite box accepted");
        var collision=BND4.Read(composedBank);collision.Files.Add(new BinderFile(collision.Files[0].Flags,99991,"f000824102.fxr",new byte[]{0}));var collisionBytes=collision.Write();
        Reject(()=>BossFogWalls.Apply(Native,p=>p==bankPath?collisionBytes:Composed(p),clean,walls),"Conflicting SFX resource accepted");
        Check(clean.Write().SequenceEqual(pristine),"Rejected fog mutated the caller MSB");
        Check(BossFogWalls.Apply(_=>throw new Exception(),_=>throw new Exception(),clean,[]).Files.Count==0,"Empty fog set reads assets");
        foreach(var (path,hash) in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"Installed asset changed: "+path);
        string folder="out/map-editor/boss-encounters/fog";Directory.CreateDirectory(folder);
        File.WriteAllText(folder+"/regression.json",JsonSerializer.Serialize(new{status="PASS",sourceBounds,walls=result.Bindings,files=result.Files.ToDictionary(p=>p.Key,p=>Hash(p.Value)),sfxResources=bank.Files.Count,maxWorldError,sourceAndNativeAssetsPreserved=hashes.Count,result.Limits,result.NativeHashes},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"Boss fog PASS: 3 source boxes, {bank.Files.Count} closed SFX resources; max world delta {maxWorldError*1000:F5}mm; {hashes.Count} installed assets preserved. Native gameplay pending.");
    }
    static (List<Vector3> Vertices,List<Vector4> Planes,float Radius,HashSet<int> Allowed) Decode(byte[] b) {
        // Independently locate the typed object via virtual fixups, then read the
        // BB hkRelArray fields rather than assuming the donor's object address.
        int U(int p)=>BitConverter.ToInt32(b,p);int section=64+BitConverter.ToUInt16(b,62),names=U(section+20),header=section+128;
        int data=U(header+20),virt=U(header+32),end=U(header+36),shape=-1;
        for(int p=data+virt;p+12<=data+end;p+=12)if(U(p)!=-1) {int start=names+U(p+8);string name=Encoding.ASCII.GetString(b,start,Array.IndexOf(b,(byte)0,start)-start);if(name=="hknpConvexPolytopeShape")shape=data+U(p);}
        Check(shape>=0,"No native convex shape");var allowed=new HashSet<int>();var vertices=new List<Vector3>();var planes=new List<Vector4>();
        int ArrayAt(int field,int count) {Check(BitConverter.ToUInt16(b,shape+field)==count,"Native inline array count");return shape+field+BitConverter.ToUInt16(b,shape+field+2);}
        int v=ArrayAt(40,8),n=ArrayAt(44,8);_=ArrayAt(48,6);_=ArrayAt(52,24);
        for(int i=0;i<8;++i) {int p=v+i*16;vertices.Add(new(BitConverter.ToSingle(b,p),BitConverter.ToSingle(b,p+4),BitConverter.ToSingle(b,p+8)));foreach(int at in Enumerable.Range(p,12))allowed.Add(at);}
        for(int i=0;i<6;++i) {int p=n+i*16;planes.Add(new(BitConverter.ToSingle(b,p),BitConverter.ToSingle(b,p+4),BitConverter.ToSingle(b,p+8),BitConverter.ToSingle(b,p+12)));foreach(int at in Enumerable.Range(p+12,4))allowed.Add(at);}
        foreach(int at in Enumerable.Range(shape+20,4))allowed.Add(at);return(vertices,planes,BitConverter.ToSingle(b,shape+20),allowed);
    }
}

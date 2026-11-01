// SPDX-License-Identifier: GPL-3.0-or-later
#nullable enable
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class MapMerchantsTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static Matrix4x4 Sample(Spline.TransformTrack t,float time) {
        var p=t.StaticPosition;var s=t.StaticScale;
        if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(time)??p.X,t.SplinePosition.GetValueY(time)??p.Y,t.SplinePosition.GetValueZ(time)??p.Z);
        if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(time)??s.X,t.SplineScale.GetValueY(time)??s.Y,t.SplineScale.GetValueZ(time)??s.Z);
        var q=t.HasSplineRotation?t.SplineRotation.GetValue(time):t.StaticRotation;
        return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(p);
    }
    static float Error(Matrix4x4 a,Matrix4x4 b) {
        float[] x=[a.M11,a.M12,a.M13,a.M21,a.M22,a.M23,a.M31,a.M32,a.M33,a.M41,a.M42,a.M43],y=[b.M11,b.M12,b.M13,b.M21,b.M22,b.M23,b.M31,b.M32,b.M33,b.M41,b.M42,b.M43];
        return x.Zip(y,(x,y)=>Math.Abs(x-y)).Max();
    }
    // Inspect raw fixup targets before either production reader or decoder.
    static void BorrowedNames(byte[] bytes) {
        int sections=64+BitConverter.ToUInt16(bytes,62),data=BitConverter.ToInt32(bytes,sections+148),local=BitConverter.ToInt32(bytes,sections+152),global=BitConverter.ToInt32(bytes,sections+156);
        var fixups=new Dictionary<int,int>();for(int at=local;at+8<=global;at+=8)if(BitConverter.ToInt32(bytes,data+at)!=-1)fixups.Add(BitConverter.ToInt32(bytes,data+at),BitConverter.ToInt32(bytes,data+at+4));
        var p=new NativeAnimation.Pack(bytes);int a=p.Object("hkaSkeleton"),bones=fixups[a+40],count=BitConverter.ToInt32(bytes,data+a+48);
        foreach(int field in new[]{a+16}.Concat(Enumerable.Range(0,count).Select(i=>bones+i*16))) {
            Check(fixups.TryGetValue(field,out int target)&&(target&1)==0,"Borrowed skeleton string has native ownership bit set");
            Check(BitConverter.ToUInt64(bytes,data+field)==0,"Relocated string pointer leaked into the serialized skeleton");
        }
    }
    public static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],definitions=args[2],output=args[3];var hashes=new Dictionary<string,string>();var composed=new Dictionary<string,byte[]>();
        byte[] Read(string p){var b=File.ReadAllBytes(p);hashes.TryAdd(p,Convert.ToHexStringLower(SHA256.HashData(b)));return b;}
        byte[]? Source(string p)=>File.Exists(Path.Combine(sourceRoot,p))?Read(Path.Combine(sourceRoot,p)):null;
        byte[]? Native(string p)=>composed.GetValueOrDefault(p)??(File.Exists(Path.Combine(nativeRoot,p))?Read(Path.Combine(nativeRoot,p)):null);
        const string gamePath="param/gameparam/gameparam.parambnd.dcx";var originalGame=BND4.Read(Native(gamePath)!);var baseline=Native(gamePath)!;
        var oldRows=MapCharacters.NativeRows(originalGame.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);
        var nativeMap=MSBB.Read(Native("map/MapStudio/m21_00_00_00.msb.dcx")!);var doll=nativeMap.Parts.Enemies.Single(a=>a.ModelName=="c8030"&&a.NPCParamID==803000&&a.ThinkParamID==803000);
        var donorAnimation=BND4.Read(Native("chr/c8030.anibnd.dcx")!);var oldRig=NativeAnimation.Skeleton(donorAnimation.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
        var originalModel=FLVER2.Read(MapCharacters.Authored(Source,"c2510").Model);var results=new List<object>();int clipCount=0,poseCount=0;float maxPoseError=0;Vector3 offset=new(-143.8f,20.07f,164.63f);
        foreach(string map in new[]{"m10_02_00_00","m10_01_00_00"}) {
            bool female=map=="m10_02_00_00";int sourceEntity=female?6240:6230;string target=female?"c8903":"c8904";
            var sourceMap=MSB1.Read(Source("map/MapStudio/"+map+".msb")!);var sourceActor=sourceMap.Parts.Enemies.Single(a=>a.EntityID==sourceEntity);
            Check(sourceActor.DrawGroups.All(x=>x==0)&&sourceActor.DispGroups.All(x=>x==0),"Fixture no longer covers source invisible groups");
            var position=sourceActor.Position+offset;var destination=new MSBB();var slots=new List<MSBB.Part.Collision>();var chunks=new List<MapCollisionBindings.Chunk>();
            foreach(int i in new[]{0,1}) {
                var collision=(MSBB.Part.Collision)nativeMap.Parts.Collisions.First().DeepCopy();collision.Name="merchant_floor_"+i;collision.EntityID=4309800+i;
                Array.Clear(collision.DrawGroups);collision.DrawGroups[0]=i==1?8u:4u;Array.Clear(collision.DispGroups);collision.DispGroups[0]=i==1?16u:2u;
                destination.Parts.Collisions.Add(collision);slots.Add(collision);var center=position+(i==0?new Vector3(500,0,0):Vector3.Zero);
                MapCollision.Triangle[] floor=[new(center+new Vector3(-3,0,-3),center+new Vector3(-3,0,3),center+new Vector3(3,0,-3))];chunks.Add(new(sourceActor.CollisionName,floor,floor));
            }
            var before=baseline.ToArray();var import=MapMerchants.Bake(Source,Native,"DS1R",map,definitions,nativeRoot,destination,offset,chunks,slots,baseline);
            Check(before.SequenceEqual(baseline)&&destination.Parts.Enemies.Count==0&&destination.Models.Enemies.Count==0&&destination.Parts.Collisions.Count==2,"Merchant staging mutated caller-owned data");
            var actor=import.Actors.Single();var report=import.Reports.Single();var model=import.Models.Single();
            Check(actor.EntityID==(female?2419703:4039700)&&actor.ModelName==target&&actor.Name==target+"_0000"&&model.Name==target&&actor.NPCParamID==int.Parse(target[1..])*100,"Fixed merchant identities changed");
            Check(actor.Position==position&&actor.Rotation==sourceActor.Rotation&&actor.Scale==Vector3.One,"Source merchant transform or proportions changed");
            Check(actor.ThinkParamID==-1&&actor.TalkID==-1&&actor.CharaInitID==-1&&actor.InitAnimID==-1&&actor.DamageAnimID==-1,"Merchant copied native AI, talk or initial modifiers");
            Check(actor.CollisionName==slots[1].Name&&actor.DrawGroups.SequenceEqual(slots[1].DrawGroups)&&actor.DispGroups.SequenceEqual(slots[1].DispGroups)&&actor.BackreadGroups.SequenceEqual(doll.BackreadGroups),"Merchant uses wrong floor, invisible source groups or invalid native backread groups");
            Check(report.SourceEntityID==sourceEntity&&report.DisplayMasks.SequenceEqual(female?Array.Empty<int>():new[]{0})&&report.SourcePose.SourceClipID==(female?0:9000),"Source merchant loadout/idle identity changed");
            var bank=BND4.Read(import.Files["chr/"+target+".chrbnd.dcx"]);var flver=FLVER2.Read(bank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
            var meshes=originalModel.Meshes.Where(m=>!originalModel.Materials[m.MaterialIndex].Name.Contains("#00#",StringComparison.Ordinal)||!female).ToArray();
            Check(meshes.Length==(female?3:4)&&flver.Meshes.Count==meshes.Length,"Male katana mask leaked to female merchant");
            int active=flver.Nodes.Count(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0);
            Check(flver.Nodes.Count==185&&flver.Nodes.Take(active).All(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0)&&flver.Nodes.Skip(active).All(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)!=0),"Merchant render palette lacks a contiguous active prefix");
            for(int mesh=0;mesh<meshes.Length;mesh++)for(int v=0;v<meshes[mesh].Vertices.Count;v++) {
                var old=meshes[mesh].Vertices[v];var next=flver.Meshes[mesh].Vertices[v];Check(Vector3.Distance(old.Position,next.Position)<.0001f&&next.UVs.Count>=old.UVs.Count&&old.UVs.Zip(next.UVs).All(p=>Vector3.Distance(p.First,p.Second)<.001f)&&next.UVs.Skip(old.UVs.Count).All(v=>v==Vector3.Zero),"Authored merchant geometry/UVs changed mesh="+mesh+" vertex="+v+" pos="+Vector3.Distance(old.Position,next.Position)+" oldUV="+string.Join(",",old.UVs)+" newUV="+string.Join(",",next.UVs));
                for(int k=0;k<4;k++)if(next.BoneWeights[k]>0){int b=SkinRetarget.Bone(flver.Meshes[mesh],next.BoneIndices[k]);Check(b<active&&b<=255&&flver.Nodes[b].Name.StartsWith("DS1_",StringComparison.Ordinal),"Source merchant skin uses native guessed bones or inactive palette");}
            }
            Check(bank.Files.Any(f=>f.ID==100&&TPF.Is(f.Bytes.ToArray()))&&!bank.Files.Any(f=>Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).EndsWith(".clm2",StringComparison.OrdinalIgnoreCase)),"Merchant texture residency or cloth isolation changed");
            var animation=BND4.Read(import.Files["chr/"+target+".anibnd.dcx"]);var skeleton=animation.Files.Single(f=>f.ID==1000000).Bytes.ToArray();BorrowedNames(skeleton);
            var rig=NativeAnimation.Skeleton(skeleton,allowMirroredScale:true);var source=SourceAnimation.Load(Source,"c2510",female?0:9000);
            Check(rig.Names.Length==185&&rig.Names.Take(129).SequenceEqual(oldRig.Names)&&rig.Parents.Take(129).SequenceEqual(oldRig.Parents)&&rig.Local.Take(129).SequenceEqual(oldRig.Local),"Merchant changed original native rig indices/reference pose");
            Check(rig.Names.Skip(129).SequenceEqual(source.Bones.Select(b=>"DS1_"+b.Name)),"Source visible hierarchy is incomplete");
            foreach(var old in donorAnimation.Files.Where(f=>f.ID>=1000000000)) {
                var bytes=animation.Files.Single(f=>f.ID==old.ID).Bytes.ToArray();var op=new NativeAnimation.Pack(old.Bytes.ToArray());var np=new NativeAnimation.Pack(bytes);
                int oa=op.Object("hkaSplineCompressedAnimation"),na=np.Object("hkaSplineCompressedAnimation");var od=op.Array(oa+152,1,64*1024*1024);var nd=np.Array(na+152,1,64*1024*1024);
                var os=op.Array(oa+88,4,256);var ns=np.Array(na+88,4,256);var oe=op.Array(oa+104,4,256);int frames=op.Int(oa+56);
                Check(np.Int(na+20)==185&&np.Int(na+56)==frames&&np.Int(na+60)==op.Int(oa+60),"Merchant changed native playback frames or omitted source tracks");
                bool idle=old.ID is 1000000000 or 1001000000 or 1002000000;
                Check(Math.Abs(np.Float(na+16)-(idle?source.Duration:op.Float(oa+16)))<.00001f,"Merchant source idle duration or unrelated native timing changed");
                Check(Math.Abs(np.Float(na+80)*(frames-1)-np.Float(na+16))<.00001f&&Math.Abs(np.Float(na+72)*np.Float(na+76)-1)<.00001f,"Retimed merchant idle has inconsistent native frame timing");
                var decoded=Spline.ReadSplineCompressedAnimByteBlock(false,bytes.AsSpan(np.Data+nd.At,nd.Count).ToArray(),185,np.Int(na+60));
                for(int block=0;block<os.Count;block++) {
                    int o=op.Data+od.At+op.Int(os.At+block*4),n=np.Data+nd.At+np.Int(ns.At+block*4),used=op.Int(oe.At+block*4);
                    Check(old.Bytes.Span.Slice(o,129*4).SequenceEqual(bytes.AsSpan(n,129*4))&&old.Bytes.Span.Slice(o+129*4,used-129*4).SequenceEqual(bytes.AsSpan(n+185*4,used-129*4)),"Original compressed native body channels changed");
                    int last=Math.Min(255,frames-1-block*255);
                    foreach(int frame in new[]{0,last/2,last}) {
                        bool additive=old.Bytes.Span[op.Data+op.Object("hkaAnimationBinding")+80]==1;
                        var expected=additive?Enumerable.Repeat(Matrix4x4.Identity,56).ToArray():source.Sample((block*255f+frame)/(frames-1)*source.Duration);
                        if(!additive){Check(Matrix4x4.Invert(Sample(decoded[block][0],frame),out var inverse),"Native merchant root is singular");expected[0]*=inverse;}
                        for(int b=0;b<56;b++){float error=Error(expected[b],Sample(decoded[block][129+b],frame));maxPoseError=Math.Max(error,maxPoseError);Check(error<.0035f,"Source merchant pose not preserved: "+map+"/"+old.ID+"/"+b+"/"+error);}poseCount++;
                    }
                }
                clipCount++;
            }
            var oldTae=TAE.Read(donorAnimation.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);var newTae=TAE.Read(animation.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
            Check(newTae.ID==200000+int.Parse(target[1..]),"Merchant TAE ownership not rebased");
            foreach(var old in oldTae.Animations) {
                var next=newTae.Animations.Single(a=>a.ID==old.ID);bool idle=old.ID is 0 or 1000000 or 2000000;
                if(!idle)Check(old.Events.Count==next.Events.Count&&old.Events.Zip(next.Events).All(pair=>pair.First.Type==pair.Second.Type&&pair.First.StartTime==pair.Second.StartTime&&pair.First.EndTime==pair.Second.EndTime&&pair.First.GetParameterBytes(false).SequenceEqual(pair.Second.GetParameterBytes(false))),"Merchant changed non-idle native event payloads/timing");
                else Check(!next.Events.Any(e=>e.Type is 66 or 67 or 128 or 313 or 315)&&next.Events.All(e=>e.StartTime>=0&&e.EndTime<=source.Duration+.00001f),"Merchant inherited Doll idle modifiers/audio or wrong event duration");
            }
            Check(import.Files["action/script/"+target+".hks"].SequenceEqual(Native("action/script/c8030.hks")!),"Friendly native animation callback script changed");
            var game=BND4.Read(import.GameParamBytes);var npcs=MapCharacters.NativeRows(game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);var prior=MapCharacters.NativeRows(BND4.Read(baseline).Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);
            Check(npcs.Rows.Count==prior.Rows.Count+1&&npcs.Rows.Take(prior.Rows.Count).Zip(prior.Rows).All(p=>p.First.ID==p.Second.ID&&p.First.Name==p.Second.Name&&MapCharacters.Payload(p.First).SequenceEqual(MapCharacters.Payload(p.Second))),"Merchant parameter composition damaged original/previous imported rows");
            var payload=MapCharacters.Payload(npcs.Rows.Single(r=>r.ID==actor.NPCParamID));var expectedNpc=MapCharacters.Payload(oldRows.Rows.Single(r=>r.ID==803000));BitConverter.GetBytes(-1).CopyTo(expectedNpc,4);expectedNpc[0x149]&=0xDF;foreach(int at in new[]{0x146,0x147,0x14A,0x14B})expectedNpc[at]=255;
            Check(payload.SequenceEqual(expectedNpc)&&BitConverter.ToInt16(payload,0x162)==-1,"Merchant parameters changed beyond owned AI/language/display fields");
            foreach(var file in BND4.Read(baseline).Files.Where(f=>Leaf(f.Name)!="NpcParam.param"))Check(game.Files.Single(f=>f.ID==file.ID).Bytes.Span.SequenceEqual(file.Bytes.Span),"Merchant damaged unrelated parameter table");
            Reject(()=>MapMerchants.Bake(Source,Native,"DS1R",map,definitions,nativeRoot,destination,offset,chunks,slots,import.GameParamBytes),"Occupied merchant parameter ID accepted");
            Reject(()=>MapMerchants.Bake(Source,path=>path=="sound/sprj_"+target+"_jaj.fsb"?[1]:Native(path),"DS1R",map,definitions,nativeRoot,destination,offset,chunks,slots,baseline),"Existing localized sound namespace accepted");
            destination.Parts.Enemies.Add((MSBB.Part.Enemy)actor.DeepCopy());Reject(()=>MapMerchants.Bake(Source,Native,"DS1R",map,definitions,nativeRoot,destination,offset,chunks,slots,baseline),"Occupied merchant model/entity accepted");destination.Parts.Enemies.Clear();
            results.Add(report);baseline=import.GameParamBytes;foreach(var file in import.Files)composed[file.Key]=file.Value;
            Console.WriteLine("PASS "+map+": "+target+", sourceidle"+source.Duration+"s, 185 bones / 61 clips, correct source masks and native-friendly placement");
        }
        foreach(var (path,hash) in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==hash,"Original asset changed: "+path);
        Directory.CreateDirectory(output);File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{clipCount,poseCount,maxPoseError,originalHashes=hashes,results},new JsonSerializerOptions{IncludeFields=true,WriteIndented=true}));
        Console.WriteLine($"PASS: {clipCount} native/source clips / {poseCount} independently decoded poses, original/composed opaque parameters preserved, {hashes.Count} original hashes unchanged");
    }
}

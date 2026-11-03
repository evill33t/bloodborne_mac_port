// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

internal static class CatacombsCreaturesPhysicsTests {
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static string Hash(byte[] b)=>Convert.ToHexStringLower(SHA256.HashData(b));
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    static void Reject(Action a,string why){try{a();}catch(InvalidDataException){return;}throw new Exception("Accepted malformed physics: "+why);}
    static Matrix4x4[] AnimatedWorlds(byte[] clip,NativeAnimation.Rig rig,float fraction) {
        var pack=new NativeAnimation.Pack(clip);int a=pack.Object("hkaSplineCompressedAnimation");var data=pack.Array(a+152,1,64*1024*1024);var blocks=Spline.ReadSplineCompressedAnimByteBlock(false,clip.AsSpan(pack.Data+data.At,data.Count).ToArray(),rig.Names.Length,pack.Int(a+60));
        float frame=(pack.Int(a+56)-1)*fraction;int block=Math.Min((int)frame/255,blocks.Count-1);
        var local=blocks[block].Select(t=>{float f=frame-block*255;Vector3 p=t.StaticPosition,s=t.StaticScale;if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(f)??p.X,t.SplinePosition.GetValueY(f)??p.Y,t.SplinePosition.GetValueZ(f)??p.Z);if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(f)??s.X,t.SplineScale.GetValueY(f)??s.Y,t.SplineScale.GetValueZ(f)??s.Z);return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(t.HasSplineRotation?t.SplineRotation.GetValue(f):t.StaticRotation))*Matrix4x4.CreateTranslation(p);}).ToArray();
        return NativeAnimation.Worlds(local,rig.Parents);
    }
    static void Main(string[] args) {
        string candidate=args[0],native=args[1],source=args[2],output=args[3];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();
        byte[] Read(string p){var b=File.ReadAllBytes(p);hashes[p]=Hash(b);return b;}
        byte[]? Source(string p)=>Read(Path.Combine(source,p));
        var donor=BND4.Read(Read(Path.Combine(native,"chr/c2610.chrbnd.dcx")));var physics=donor.Files.Single(f=>f.ID==300).Bytes.ToArray();var pwv=donor.Files.Single(f=>Leaf(f.Name).EndsWith(".hkxpwv",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
        var cases=new[]{("c8840","c2900"),("c8841","c2900"),("c8842","c2900"),("c8843","c2910"),("c8844","c2650"),("c8845","c2930")};
        var sources=new Dictionary<string,CatacombsCreaturesPhysicsSource>();var reports=new List<object>();
        foreach(var (target,model) in cases) {
            if(!sources.TryGetValue(model,out var authored)){authored=CatacombsCreaturesPhysicsSource.Load(Source,model);sources.Add(model,authored);}
            var animation=BND4.Read(Read(Path.Combine(candidate,"dvdroot_ps4/chr/"+target+".anibnd.dcx")));var rig=animation.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
            var result=CatacombsCreaturesPhysics.Convert(physics,pwv,authored,rig);var parsed=new HollowDeathRig.Pack(result.Physics);int system=parsed.One("hknpRagdollData");var bodies=parsed.Array(system+64,96);var constraints=parsed.Array(system+80,24);
            Check(bodies.Count==authored.Bodies.Count&&constraints.Count==authored.Constraints.Count,"physical population changed");
            Check(parsed.Objects.Count(o=>o.Name=="hknpCapsuleShape")==result.Report.Capsules&&parsed.Objects.Count(o=>o.Name=="hknpConvexPolytopeShape")==result.Report.Boxes,"exact native capsule/box class identities");
            Check(!parsed.Objects.Any(o=>o.Name=="hkpBreakableConstraintData"),"unverified physical breakable wrapper emitted");
            var skeletons=parsed.Objects.Where(o=>o.Name=="hkaSkeleton").Select(o=>(o.At,Info:parsed.Skeleton(o.At))).ToArray();var full=skeletons.Single(s=>s.Info.Names.Length==result.Report.AnimationBones);var rag=skeletons.Single(s=>s.Info.Names.SequenceEqual(authored.Ragdoll.Names));
            Check(full.Info.Names.Skip(80).SequenceEqual(authored.Animation.Names.Select(n=>"DS1_"+n)),"source visible bone closure changed");
            var forward=parsed.Objects.Single(o=>o.Name=="hkaSkeletonMapper"&&parsed.Pointer(o.At+16)==full.At);var sm=parsed.Array(forward.At+80,64);var cm=parsed.Array(forward.At+96,112);
            var originalForward=authored.Mappings.Single(m=>m.B.Names.SequenceEqual(authored.Ragdoll.Names));
            Check(sm.Count==originalForward.Simple.Count&&cm.Count==originalForward.Chains.Count,"source mapper chain omitted");
            void MappingContract(int at,CatacombsCreaturesPhysicsSource.Mapping original,int offsetA,int offsetB) {
                var simple=parsed.Array(at+80,64);var chains=parsed.Array(at+96,112);var unmapped=parsed.Array(at+112,2);
                Check(simple.Count==original.Simple.Count&&chains.Count==original.Chains.Count&&parsed.Raw(at+128,48).SequenceEqual(original.Extracted),"mapper/extracted-motion ABI changed");
                for(int i=0;i<simple.Count;i++){int p=simple.At+i*64;var s=original.Simple[i];Check(BitConverter.ToInt16(result.Physics,parsed.Data+p)==s.A+offsetA&&BitConverter.ToInt16(result.Physics,parsed.Data+p+2)==s.B+offsetB&&parsed.Raw(p+16,48).SequenceEqual(s.Transform),"ordered source simple mapping changed");}
                for(int i=0;i<chains.Count;i++){int p=chains.At+i*112;var c=original.Chains[i];Check(new[]{(int)BitConverter.ToInt16(result.Physics,parsed.Data+p),(int)BitConverter.ToInt16(result.Physics,parsed.Data+p+2),(int)BitConverter.ToInt16(result.Physics,parsed.Data+p+4),(int)BitConverter.ToInt16(result.Physics,parsed.Data+p+6)}.SequenceEqual(new[]{c.StartA+offsetA,c.EndA+offsetA,c.StartB+offsetB,c.EndB+offsetB})&&parsed.Raw(p+16,96).SequenceEqual(c.Transforms),"authored mapper chain changed");}
                var expected=Enumerable.Range(0,offsetB).Concat(original.Unmapped.Select(i=>i+offsetB)).ToArray();Check(unmapped.Count==expected.Length&&Enumerable.Range(0,unmapped.Count).Select(i=>(int)BitConverter.ToInt16(result.Physics,parsed.Data+unmapped.At+i*2)).SequenceEqual(expected),"controller/source unmapped closure changed");
            }
            MappingContract(forward.At,originalForward,80,0);var backward=parsed.Objects.Single(o=>o.Name=="hkaSkeletonMapper"&&parsed.Pointer(o.At+16)==rag.At);MappingContract(backward.At,authored.Mappings.Single(m=>m.A.Names.SequenceEqual(authored.Ragdoll.Names)),0,80);
            var pose=NativeAnimation.Worlds(full.Info.Local,full.Info.Parents.Select(i=>(int)i).ToArray());float maxReference=0,maxBody=0;
            foreach(var body in authored.Bodies) {
                int at=bodies.At+body.Index*96;Check(parsed.Text(at+32)==body.Name,"body name changed");int shape=parsed.Pointer(at);float radius=BitConverter.ToSingle(result.Physics,parsed.Data+shape+20);Check(radius==body.Shape.Radius,"source convex radius changed");
                Vector3 Vec(int field)=>new(BitConverter.ToSingle(result.Physics,parsed.Data+field),BitConverter.ToSingle(result.Physics,parsed.Data+field+4),BitConverter.ToSingle(result.Physics,parsed.Data+field+8));
                var q=new Quaternion(Vec(at+64),BitConverter.ToSingle(result.Physics,parsed.Data+at+76));var rest=Matrix4x4.CreateFromQuaternion(q)*Matrix4x4.CreateTranslation(Vec(at+48));maxBody=Math.Max(maxBody,CatacombsCreaturesPhysics.Error(rest,body.Rest));
                if(body.Shape.Type=="hkpCapsuleShape")Check(Vector3.Distance(Vec(shape+64),body.Shape.A+body.Shape.Translation)<.000001f&&Vector3.Distance(Vec(shape+80),body.Shape.B+body.Shape.Translation)<.000001f,"source capsule endpoints changed");
                else {int vertices=shape+40+BitConverter.ToUInt16(result.Physics,parsed.Data+shape+42);var points=Enumerable.Range(0,8).Select(i=>Vec(vertices+i*16)).ToArray();Check(Vector3.Distance(points.Aggregate(Vector3.Min),body.Shape.Translation-body.Shape.Half)<.00001f&&Vector3.Distance(points.Aggregate(Vector3.Max),body.Shape.Translation+body.Shape.Half)<.00001f,"source translated box extent changed");}
                int bone=Array.IndexOf(authored.BoneToBody,body.Index);var mappings=Enumerable.Range(0,sm.Count).Select(i=>sm.At+i*64).Where(p=>BitConverter.ToInt16(result.Physics,parsed.Data+p+2)==bone).ToArray();Check(mappings.Length>=1,"physical body lacks exact visible bone mapper");foreach(int m in mappings){int mapped=BitConverter.ToInt16(result.Physics,parsed.Data+m);Check(mapped>=80,"donor anatomical hurtbox retained");maxReference=Math.Max(maxReference,CatacombsCreaturesPhysics.Error(parsed.Qs(m+16)*pose[mapped],body.Rest));}
            }
            Check(maxBody<.0001f&&maxReference<.003f,"body rest/visible bind disagree");
            for(int i=0;i<constraints.Count;i++) {
                int at=constraints.At+i*24,child=parsed.Pointer(at);var expected=HollowDeathRig.ConstraintData(authored.Constraints[i].Child);
                Check(parsed.Int(at+8)==authored.Constraints[i].Child.A&&parsed.Int(at+12)==authored.Constraints[i].Child.B,"source joint topology changed");
                Check(parsed.Objects.Any(o=>o.At==child&&o.Name==authored.Constraints[i].Child.Type)&&parsed.Raw(child,expected.Bytes.Length).SequenceEqual(expected.Bytes),"source child joint frames/limits/motor ABI changed");
            }
            var fullRig=NativeAnimation.Skeleton(rig,true);var sourceClip=SourceAnimation.Load(Source,model,0);var authoredForward=authored.Mappings.Single(m=>m.B.Names.SequenceEqual(authored.Ragdoll.Names));float worstAnimated=0;
            var posedBounds=new List<object>();
            foreach(float fraction in new[]{0f,.25f,.5f,.75f,1f}) {
                var nativePose=AnimatedWorlds(animation.Files.Single(f=>f.ID==1000000000).Bytes.ToArray(),fullRig,fraction);var sourcePose=NativeAnimation.Worlds(sourceClip.Sample(sourceClip.Duration*fraction),sourceClip.Bones.Select(b=>b.Parent).ToArray());
                var bodyPositions=new List<Vector3>();var headBounds=new List<Vector3>();
                for(int i=0;i<sm.Count;i++) {
                    int at=sm.At+i*64,a=BitConverter.ToInt16(result.Physics,parsed.Data+at),z=BitConverter.ToInt16(result.Physics,parsed.Data+at+2);var expected=authoredForward.Simple[i];
                    Check(a==expected.A+80&&z==expected.B,"serialized ordered source mapper changed");var actual=parsed.Qs(at+16)*nativePose[a];var sourceExpected=HollowDeathRig.QsMatrix(expected.Transform)*sourcePose[expected.A];worstAnimated=Math.Max(worstAnimated,CatacombsCreaturesPhysics.Error(actual,sourceExpected));
                    int body=authored.BoneToBody[z];if(body>=0){var shape=authored.Bodies[body].Shape;if(shape.Type=="hkpCapsuleShape"){var start=Vector3.Transform(shape.A+shape.Translation,actual);var end=Vector3.Transform(shape.B+shape.Translation,actual);bodyPositions.Add(start);bodyPositions.Add(end);if(authored.Bodies[body].Name.Contains("Head",StringComparison.OrdinalIgnoreCase))headBounds.AddRange(new[]{start-new Vector3(shape.Radius),start+new Vector3(shape.Radius),end-new Vector3(shape.Radius),end+new Vector3(shape.Radius)});}}
                }
                posedBounds.Add(new{Fraction=fraction,BodyEndpointMin=bodyPositions.Aggregate(Vector3.Min),BodyEndpointMax=bodyPositions.Aggregate(Vector3.Max),HeadCapsuleMin=headBounds.Count==0?(Vector3?)null:headBounds.Aggregate(Vector3.Min),HeadCapsuleMax=headBounds.Count==0?(Vector3?)null:headBounds.Aggregate(Vector3.Max)});
            }
            Check(worstAnimated<.003f,"animated native body follows donor or wrong source pose: "+worstAnimated);
            var outPwv=HKXPWV.Read(result.Pwv);var nativePwv=HKXPWV.Read(pwv);Check(outPwv.Write().SequenceEqual(result.Pwv)&&outPwv.AnimBoneEntries.Count==result.Report.AnimationBones&&outPwv.BoneMapEntries.Count==bodies.Count&&outPwv.RagdollBoneEntries.Count==authored.Ragdoll.Names.Length,"full native PWV sizes/roundtrip");
            Check(outPwv.AnimBoneEntries.Take(80).Select(r=>(r.SkeletonParamID,r.Unk2)).SequenceEqual(nativePwv.AnimBoneEntries.Select(r=>(r.SkeletonParamID,r.Unk2))),"native controller SkeletonParam records changed");
            Check(result.Report.SourcePwvHeaderPolicy==authored.PwvHeaderPolicy&&result.Report.NativePwvHeaderPolicy==0&&BitConverter.ToUInt32(result.Pwv,12)==BitConverter.ToUInt32(pwv,12)&&outPwv.RagdollBoneEntries.All(r=>r.DamageAnimID==-1),"PWV native policy/source report/dangling damage animation");
            Check(outPwv.RagdollBoneEntries.Select(r=>r.NPCPartGroupIndex).SequenceEqual(authored.Pwv.RagdollBoneEntries.Select(r=>r.NPCPartGroupIndex)),"source part group discarded");
            File.WriteAllBytes(Path.Combine(output,target+".hkx"),result.Physics);File.WriteAllBytes(Path.Combine(output,target+".hkxpwv"),result.Pwv);reports.Add(new{Target=target,result.Report,WorstBodyRestError=maxBody,WorstVisibleMapperReferenceError=maxReference,WorstAnimatedBodyError=worstAnimated,AnimatedBounds=posedBounds});
            if(target=="c8840"){
                var bank=BND3.Read(Source("chr/c2900.chrbnd.dcx")!);var raw=bank.Files.Single(f=>Leaf(f.Name)=="c2900.hkx").Bytes.ToArray();var metadata=bank.Files.Single(f=>Leaf(f.Name)=="c2900.hkxpwv").Bytes.ToArray();var tag=new SourceCollision.Tag(raw);var cap=tag.Items.First(i=>i.Count==1&&tag.Name(i.Type)=="hkpCapsuleShape");var changed=raw.ToArray();BitConverter.GetBytes(float.NaN).CopyTo(changed,tag.Nested(cap.Type,cap.At,"radius").At);
                Reject(()=>new CatacombsCreaturesPhysicsSource(model,changed,metadata),"nonfinite actual source capsule");
                changed=metadata.ToArray();BitConverter.GetBytes((short)99).CopyTo(changed,32+authored.Bodies.Count*4);Reject(()=>new CatacombsCreaturesPhysicsSource(model,raw,changed),"unowned source SkeletonParam");
                Reject(()=>CatacombsCreaturesPhysics.Convert(physics,pwv,authored,donor.Files.First(f=>f.ID==300).Bytes.ToArray()),"physics used as playback skeleton");
                Reject(()=>CatacombsCreaturesPhysics.Convert(physics,pwv[..^1],authored,rig),"truncated native PWV");
            }
        }
        foreach(var(path,hash)in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"original/candidate archive changed: "+path);
        File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{Status="PASS_FIXTURE_ONLY",OriginalArchivesUnchanged=hashes.Count,Hashes=hashes,Cases=reports,GameplayTested=false},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS Catacombs physics fixtures: all6 core loadouts /4 source families, exact source body+mapper shapes/PWV, no donor hurtboxes or dangling a99 reactions, {hashes.Count} inputs unchanged; native solver/contact/reassembly remain unproven");
    }
}

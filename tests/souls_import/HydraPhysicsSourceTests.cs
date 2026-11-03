// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class HydraPhysicsSourceTests {
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] bytes)=>Convert.ToHexStringLower(SHA256.HashData(bytes));
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{a.M11-b.M11,a.M12-b.M12,a.M13-b.M13,a.M21-b.M21,a.M22-b.M22,a.M23-b.M23,a.M31-b.M31,a.M32-b.M32,a.M33-b.M33,a.M41-b.M41,a.M42-b.M42,a.M43-b.M43}.Max(Math.Abs);
    static Matrix4x4[] Worlds(HydraPhysicsSource.Skeleton rig){var world=rig.Local.ToArray();for(int i=0;i<world.Length;i++)if(rig.Parents[i]>=0)world[i]*=world[rig.Parents[i]];return world;}
    static void Reject(Action a,string why){try{a();}catch(InvalidDataException){return;}throw new Exception("Accepted malformed Hydra "+why);}
    static void Main(string[] args) {
        string root=Path.GetFullPath(args[0]),output=Path.GetFullPath(args[1]);Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();
        byte[]? Read(string path){string p=Path.Combine(root,path);if(!File.Exists(p))return null;var b=File.ReadAllBytes(p);hashes[p]=Hash(b);return b;}
        var source=HydraPhysicsSource.Load(Read);var bank=BND3.Read(Read("chr/c3530.chrbnd.dcx")!);var physics=bank.Files.Single(f=>Leaf(f.Name)=="c3530.hkx").Bytes.ToArray();var pwv=bank.Files.Single(f=>Leaf(f.Name)=="c3530.hkxpwv").Bytes.ToArray();
        Check(source.Bodies.Count==97&&source.Constraints.Count==96&&source.Animation.Names.Length==246&&source.Ragdoll.Names.Length==97,"Source physical populations changed");
        Check(source.Bodies.All(b=>b.Name==source.Ragdoll.Names[b.Index]&&b.AnimationBone>=0&&b.AnimationBone<246),"Named animation/physics ownership missing");
        Check(source.Constraints.Count(c=>c.Type=="hkpHingeConstraintData")==95&&source.Constraints.Count(c=>c.Type=="hkpRagdollConstraintData")==1,"Plain hinges were lost or approximated");
        Check(Math.Abs(source.Bodies.Min(b=>b.Capsule.Radius)-.52477777f)<.000001f&&Math.Abs(source.Bodies.Max(b=>b.Capsule.Radius)-6.2699385f)<.000001f,"Full source capsule scale was not retained");
        var rigWorld=Worlds(source.Ragdoll);float worstBody=0,worstPivot=0,worstMapping=0;
        foreach(var body in source.Bodies) {
            worstBody=Math.Max(worstBody,Error(rigWorld[body.Index],body.Rest));
            Check(body.Capsule.Radius>0&&float.IsFinite(Vector3.Transform(body.Capsule.A,body.Rest).LengthSquared())&&float.IsFinite(Vector3.Transform(body.Capsule.B,body.Rest).LengthSquared()),"Invalid world capsule geometry");
        }
        // Independent physical invariant: both stored local joint pivots must
        // meet when placed in the original body reference frames.
        foreach(var joint in source.Constraints) {
            float gap=Vector3.Distance((joint.FrameA*source.Bodies[joint.A].Rest).Translation,(joint.FrameB*source.Bodies[joint.B].Rest).Translation);worstPivot=Math.Max(worstPivot,gap);
            Check(gap<.0002f,"Source joint pivots do not meet");
        }
        foreach(var m in source.Mappings) {
            var aw=Worlds(m.A);var bw=Worlds(m.B);
            // Long source neck hierarchies accumulate their authored float
            // reference-pose error; require agreement within one millimeter.
            foreach(var map in m.Simple){float error=Error(map.Transform*aw[map.A],bw[map.B]);worstMapping=Math.Max(worstMapping,error);Check(error<.001f,"Original bidirectional mapper bind frames disagree");}
        }
        Check(worstBody<.0002f,"Body placement differs from named ragdoll bind skeleton");
        Check(source.Mappings.Single(m=>m.B.Names.Length==246).Unmapped.Length==149&&source.PositionMotors.Count==1,"Source support bones/motor ownership changed");
        int[][] expected=[[57,69,87,90],[64,75,78,91],[58,67,79,96],[62,68,88,94],[54,77,85,95],[61,70,82,92],[56,74,86,93]];
        for(int i=0;i<7;i++){var part=source.Parts[i];Check(part.BodyIndices.Order().SequenceEqual(expected[i].Order())&&part.SourcePartID==3530+i&&part.SourceDetachedEntity==1200011+i&&part.SourceBodyEntity==1200010&&part.HP==176&&part.DisplayMask==i&&part.CollisionMask==i+1&&part.SeverSocket==91+i,"Sever branch/body ownership changed");}
        Check(source.Parts.SelectMany(p=>p.BodyIndices).Distinct().Count()==28&&source.RagdollRecords.Count(p=>p.Group==0)==69,"Source part/unowned body coverage changed");
        Check(source.Bodies.All(b=>b.CollisionFilter==0),"Unexpected source filter; do not infer groups from Havok filter bits");
        var tag=new SourceCollision.Tag(physics);var capsule=tag.Items.First(i=>i.Count==1&&tag.Name(i.Type)=="hkpCapsuleShape");
        byte[] ChangedPhysics(int at,byte[] value){var b=physics.ToArray();value.CopyTo(b,at);return b;}
        Reject(()=>new HydraPhysicsSource(physics,pwv[..^1]),"PWV truncation");
        var bad=pwv.ToArray();BitConverter.GetBytes((ushort)110).CopyTo(bad,8);Reject(()=>new HydraPhysicsSource(physics,bad),"native animation population in source PWV");
        bad=pwv.ToArray();BitConverter.GetBytes(7).CopyTo(bad,2388+57*8+4);Reject(()=>new HydraPhysicsSource(physics,bad),"unowned head group7");
        bad=pwv.ToArray();BitConverter.GetBytes(8).CopyTo(bad,2388+57*8+4);Reject(()=>new HydraPhysicsSource(physics,bad),"wrong named branch assignment");
        int radius=tag.Nested(capsule.Type,capsule.At,"radius").At;Reject(()=>new HydraPhysicsSource(ChangedPhysics(radius,BitConverter.GetBytes(float.NaN)),pwv),"nonfinite capsule");Reject(()=>new HydraPhysicsSource(ChangedPhysics(radius,BitConverter.GetBytes(0f)),pwv),"zero capsule");
        var rig=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkaRagdollInstance");var mapArray=tag.Array(rig,"boneToRigidBodyMap");Reject(()=>new HydraPhysicsSource(ChangedPhysics(tag.Element(mapArray,1).At,BitConverter.GetBytes(0)),pwv),"duplicate physical bone ownership");
        var constraint=tag.PointerArray(rig,"constraints")[0];int entities=tag.Nested(constraint.Type,constraint.At,"entities").At;Reject(()=>new HydraPhysicsSource(ChangedPhysics(entities+8,physics.AsSpan(entities,8).ToArray()),pwv),"self constraint");
        var data=tag.Reference(tag.Nested(constraint.Type,constraint.At,"data").At);int axis=tag.Nested(data.Type,data.At,"atoms","2dAng","freeRotationAxis").At;Reject(()=>new HydraPhysicsSource(ChangedPhysics(axis,[3]),pwv),"invalid free hinge axis");
        var events=EMEVD.Read(Read("event/m12_00_00_00.emevd.dcx")!);var init=events.Events.Single(e=>e.ID==0).Instructions.Single(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length==32&&BitConverter.ToInt32(i.ArgData,4)==11205300&&BitConverter.ToInt32(i.ArgData,0)==0);init.ArgData[8]=7;var badEvents=events.Write();
        Reject(()=>HydraPhysicsSource.Load(p=>p=="event/m12_00_00_00.emevd.dcx"?badEvents:Read(p)),"event group disagrees with PWV");
        Check(Hash(physics)==source.PhysicsSHA256&&Hash(pwv)==source.PwvSHA256,"Reader mutated member input");
        foreach(var(path,hash)in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"Original source archive changed: "+path);
        var json=new JsonSerializerOptions{WriteIndented=true,IncludeFields=true};
        File.WriteAllText(Path.Combine(output,"profile.json"),JsonSerializer.Serialize(new{source.PhysicsSHA256,source.PwvSHA256,source.Animation,source.Ragdoll,source.BoneToBody,source.Bodies,source.Constraints,source.Mappings,source.PositionMotors,source.BodyFlags,source.AnimationRecords,source.RagdollRecords,source.Parts},json));
        File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{status="PASS_SOURCE_READER_ONLY",bodies=97,capsules=97,constraints=96,plainHinges=95,ragdollJoints=1,animationBones=246,ragdollBones=97,partCapsules=28,unownedCapsules=69,worstBody,worstPivot,worstMapping,unchangedSourceArchives=hashes.Count,negativeCases=10,hashes,nativeConversion=false,gameplay=false},json));
        Console.WriteLine($"PASS Hydra source reader: 97 capsules, 95 hinges +1 ragdoll, 246/97 bones, both mappers, 28 head part capsules, sever-event identities; bind/pivot errors {worstBody}/{worstPivot}/{worstMapping}; {hashes.Count} source archives unchanged. Native physics/gameplay not produced.");
    }
}

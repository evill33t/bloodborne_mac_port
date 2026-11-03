// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

// Independently decode serialized records and compare geometry/ownership, not
// merely the converter's report. Inputs are the installed original archives.
static class HydraPhysicsRigTests {
    static void Check(bool value,string why) {if(!value)throw new Exception(why);}
    static void Reject(Action action,string why) {try{action();}catch(InvalidDataException){return;}throw new Exception("Accepted invalid "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] b)=>Convert.ToHexStringLower(SHA256.HashData(b));
    static float F(HollowDeathRig.Pack p,int at)=>BitConverter.ToSingle(p.Bytes,p.Data+at);
    static short S(HollowDeathRig.Pack p,int at)=>BitConverter.ToInt16(p.Bytes,p.Data+at);
    static Vector3 V(HollowDeathRig.Pack p,int at)=>new(F(p,at),F(p,at+4),F(p,at+8));
    static Quaternion Q(HollowDeathRig.Pack p,int at)=>new(F(p,at),F(p,at+4),F(p,at+8),F(p,at+12));
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(v=>Vector3.Distance(Vector3.Transform(v,a),Vector3.Transform(v,b)));
    static Matrix4x4 Frame(HollowDeathRig.Pack p,int at)=>new(F(p,at),F(p,at+4),F(p,at+8),0,F(p,at+16),F(p,at+20),F(p,at+24),0,F(p,at+32),F(p,at+36),F(p,at+40),0,F(p,at+48),F(p,at+52),F(p,at+56),1);
    static (string[] Names,int[] Parents,Matrix4x4[] Local) Skeleton(HollowDeathRig.Pack p,int at) {
        var bones=p.Array(at+40,16,512);var parents=p.Array(at+24,2,512);var pose=p.Array(at+56,48,512);
        Check(bones.Count==parents.Count&&bones.Count==pose.Count,"Serialized skeleton array disagreement");
        return(Enumerable.Range(0,bones.Count).Select(i=>p.Text(bones.At+i*16)!).ToArray(),Enumerable.Range(0,bones.Count).Select(i=>(int)S(p,parents.At+i*2)).ToArray(),Enumerable.Range(0,bones.Count).Select(i=>p.Qs(pose.At+i*48)).ToArray());
    }
    static uint Signature(byte[] raw,string name) {
        int section=64+BitConverter.ToUInt16(raw,62),names=BitConverter.ToInt32(raw,section+20),header=section+128,data=BitConverter.ToInt32(raw,header+20),v=BitConverter.ToInt32(raw,header+32),end=BitConverter.ToInt32(raw,header+36);
        for(int i=v;i+12<=end;i+=12){if(BitConverter.ToInt32(raw,data+i)==-1)continue;int n=names+BitConverter.ToInt32(raw,data+i+8);int stop=Array.IndexOf(raw,(byte)0,n);if(System.Text.Encoding.ASCII.GetString(raw,n,stop-n)==name)return BitConverter.ToUInt32(raw,n-5);}
        throw new Exception("Missing class signature "+name);
    }
    static object Validate(byte[] raw,HydraPhysicsSource source,NativeRigAppend.Plan plan,SourceAnimation attack,byte[] hingePrecedent) {
        var p=new HollowDeathRig.Pack(raw);var precedent=new HollowDeathRig.Pack(hingePrecedent);
        Check(Signature(raw,"hkpHingeConstraintData")==Signature(hingePrecedent,"hkpHingeConstraintData"),"Native hinge class signature differs");
        var shipped=precedent.Objects.Where(o=>o.Name=="hkpHingeConstraintData").ToArray();Check(shipped.Length==3,"Native c1130 plain hinge precedent changed");
        foreach(var o in shipped)foreach(var (at,type) in new[]{(32,2),(176,23),(192,12),(208,5)})Check(S(precedent,o.At+at)==type,"Native 224-byte plain hinge atom ABI differs");
        int system=p.One("hknpRagdollData"),scene=p.One("hknpPhysicsSceneData"),root=p.One("hkRootLevelContainer");var variants=p.Array(root,24,8);
        Check(variants.Count==5&&p.Pointer(variants.At+2*24+16)==system&&p.Pointer(p.Array(scene+16,8,1).At)==system,"Scene/ragdoll shared root contract");
        var bodies=p.Array(system+64,96,128);var constraints=p.Array(system+80,24,128);var motion=p.Array(system+48,112,128);var material=p.Array(system+16,80,128);var properties=p.Array(system+32,64,128);var map=p.Array(system+128,4,128);
        Check(bodies.Count==97&&constraints.Count==96&&motion.Count==97&&material.Count==97&&properties.Count==97&&map.Count==97,"Source physical populations lost");
        Check(Enumerable.Range(0,97).Select(i=>p.Int(map.At+i*4)).SequenceEqual(source.BoneToBody),"Bone/body map changed");
        var worlds=new Matrix4x4[97];float maxPivot=0,maxMapper=0,maxCapsule=0;
        for(int i=0;i<97;i++) {
            var b=source.Bodies[i];int at=bodies.At+i*96,ma=motion.At+i*112,pr=properties.At+i*64;
            worlds[i]=Matrix4x4.CreateFromQuaternion(Q(p,at+64))*Matrix4x4.CreateTranslation(V(p,at+48));
            Check(p.Text(at+32)==b.Name&&Error(worlds[i],b.Rest)<.0001f,"Source body rest/name changed");
            Check(p.Int(at+12)==i&&S(p,at+18)==i&&p.Int(at+20)==0,"Source body indices/filter changed");
            Check(F(p,ma+4)==b.InertiaMassInverse.W&&Vector3.Distance(V(p,ma+32),new(b.InertiaMassInverse.X,b.InertiaMassInverse.Y,b.InertiaMassInverse.Z))<.000001f,"Source mass/inertia changed");
            Check(Vector3.Distance(V(p,ma+48),new(b.CenterOfMass.X,b.CenterOfMass.Y,b.CenterOfMass.Z))<.000001f&&Math.Abs(Quaternion.Dot(Q(p,ma+64),b.Orientation))>.99999f,"Source motion frame changed");
            Check(F(p,pr+8)==b.Gravity&&F(p,pr+12)==b.TimeFactor&&F(p,pr+24)==b.LinearDamping&&F(p,pr+28)==b.AngularDamping,"Source motion coefficients changed");
            int shape=p.Pointer(at);Check(p.Objects.Single(o=>o.At==shape).Name=="hknpCapsuleShape","A source capsule was replaced with another shape");
            float e=Math.Max(Vector3.Distance(V(p,shape+64),b.Capsule.A),Vector3.Distance(V(p,shape+80),b.Capsule.B));maxCapsule=Math.Max(maxCapsule,e);
            Check(e<.000001f&&F(p,shape+20)==b.Capsule.Radius,"Source capsule dimensions changed");
            int vertices=shape+40+BitConverter.ToUInt16(p.Bytes,p.Data+shape+42),planes=shape+44+BitConverter.ToUInt16(p.Bytes,p.Data+shape+46);
            var points=Enumerable.Range(0,8).Select(n=>V(p,vertices+n*16)).ToArray();
            for(int n=0;n<6;n++){int pa=planes+n*16;var normal=V(p,pa);float d=F(p,pa+12);Check(Math.Abs(normal.Length()-1)<.0001f&&points.All(v=>Vector3.Dot(normal,v)+d<.001f),"Converted capsule convex support planes invalid");}
        }
        for(int i=0;i<96;i++) {
            var c=source.Constraints[i];int at=constraints.At+i*24,data=p.Pointer(at);string type=p.Objects.Single(o=>o.At==data).Name;
            Check(type==c.Type&&p.Int(at+8)==c.A&&p.Int(at+12)==c.B&&p.Int(at+16)==0, $"Source joint {i} type/topology changed: {type}/{c.Type}, {p.Int(at+8)}/{c.A}, {p.Int(at+12)}/{c.B}, flags{p.Int(at+16)}");
            Check(Error(Frame(p,data+48),c.FrameA)<.000001f&&Error(Frame(p,data+112),c.FrameB)<.000001f,"Source joint frames changed");
            Check(S(p,data+32)==2&&S(p,data+176)==23&&p.Bytes[p.Data+data+178]==(c.Stabilization.Enabled?1:0)&&F(p,data+180)==c.Stabilization.MaxLinearImpulse&&F(p,data+184)==c.Stabilization.MaxAngularImpulse&&F(p,data+188)==c.Stabilization.MaxAngle,"Source stabilization changed");
            int ball=type=="hkpHingeConstraintData"?208:400;
            Check(S(p,data+ball)==5&&p.Bytes[p.Data+data+ball+2]==c.BallSocket.SolvingMethod&&p.Bytes[p.Data+data+ball+3]==c.BallSocket.BodiesToNotify&&p.Bytes[p.Data+data+ball+4]==c.BallSocket.VelocityStabilizationFactor&&F(p,data+ball+8)==c.BallSocket.BreachImpulse&&F(p,data+ball+12)==c.BallSocket.InertiaStabilizationFactor,"Source ball-socket fields changed");
            if(type=="hkpHingeConstraintData")Check(S(p,data+192)==12&&p.Bytes[p.Data+data+194]==c.FreeRotationAxis,"Hinge free axis/native atom layout changed");
            else {
                Check(S(p,data+192)==19&&S(p,data+288)==17&&p.Pointers.Keys.Count(a=>a>=data+256&&a<data+280)==3,"Native ragdoll atom/motor pointer layout changed");
                for(int n=0;n<3;n++){int la=data+304+n*32;var limit=c.Limits[n];Check(S(p,la)==(n==0?15:16)&&F(p,la+8)==limit.Min&&F(p,la+12)==limit.Max&&F(p,la+16)==limit.Tau,"Ragdoll source limits changed");Check(p.Pointer(data+256+n*8)==p.One("hkpPositionConstraintMotor"),"Shared motor not preserved");}
            }
            float pivot=Vector3.Distance((Frame(p,data+48)*worlds[c.A]).Translation,(Frame(p,data+112)*worlds[c.B]).Translation);maxPivot=Math.Max(maxPivot,pivot);Check(pivot<.001f,"Source joint pivots diverged");
        }
        Check(p.Objects.Count(o=>o.Name=="hkpHingeConstraintData")==95&&p.Objects.Count(o=>o.Name=="hkpRagdollConstraintData")==1&&p.Objects.Count(o=>o.Name=="hkpPositionConstraintMotor")==1,"Source constraint populations lost");
        int motor=p.One("hkpPositionConstraintMotor");var sm=source.PositionMotors.Single();Check(F(p,motor+24)==sm.MinForce&&F(p,motor+28)==sm.MaxForce&&F(p,motor+32)==sm.Tau&&F(p,motor+36)==sm.Damping,"Source motor fields changed");
        int prefixUnmapped=0,sourceUnmapped=0;var forward=p.Objects.Single(o=>o.Name=="hkaSkeletonMapper"&&Skeleton(p,p.Pointer(o.At+16)).Names.Length==356);
        foreach(var o in p.Objects.Where(o=>o.Name=="hkaSkeletonMapper")) {
            var a=Skeleton(p,p.Pointer(o.At+16));var z=Skeleton(p,p.Pointer(o.At+24));var aw=NativeAnimation.Worlds(a.Local,a.Parents);var zw=NativeAnimation.Worlds(z.Local,z.Parents);
            var simple=p.Array(o.At+80,64,128);var chains=p.Array(o.At+96,112,1);var unmap=p.Array(o.At+112,2,512);Check(simple.Count==97&&chains.Count==0&&p.Bytes[p.Data+o.At+176]==1&&p.Int(o.At+180)==0,"Source mapper mode/population changed");
            var mapped=new HashSet<int>();for(int i=0;i<97;i++){int at=simple.At+i*64,aa=S(p,at),bb=S(p,at+2);Check(aa>=0&&aa<a.Names.Length&&bb>=0&&bb<z.Names.Length&&mapped.Add(bb),"Mapper indices/collision invalid");float e=Error(p.Qs(at+16)*aw[aa],zw[bb]);maxMapper=Math.Max(maxMapper,e);Check(e<.001f,"Source mapper bind differs");}
            var un=Enumerable.Range(0,unmap.Count).Select(i=>(int)S(p,unmap.At+i*2)).ToArray();Check(un.Length+mapped.Count==z.Names.Length&&un.Distinct().Count()==un.Length&&un.All(i=>i>=0&&i<z.Names.Length&&!mapped.Contains(i)),"Mapper destination coverage invalid");
            Check(p.Raw(o.At+128,48).All(b=>b==0),"Source unused extracted-motion QS invented");
            if(z.Names.Length==356){prefixUnmapped=un.Count(i=>i<110);sourceUnmapped=un.Count(i=>i>=110);Check(prefixUnmapped==110&&sourceUnmapped==149,"Controller/source unmapped support bones lost");}
        }
        // Three real source attack poses project all serialized colliders through
        // the emitted forward mapper and are compared with source mapper math.
        float maxPose=0;var fm=p.Array(forward.At+80,64,128);var sourceForward=source.Mappings.Single(m=>m.A.Names.Length==246);
        foreach(float t in new[]{0,attack.Duration*.5f,attack.Duration}) {
            var local=attack.Sample(t);var sourceWorld=NativeAnimation.Worlds(local,attack.Bones.Select(b=>b.Parent).ToArray());
            var composite=plan.Original.Local.Concat(local).ToArray();Check(Matrix4x4.Invert(plan.Original.Local[0],out var inv),"Singular controller root");composite[110]*=inv;
            var fullWorld=NativeAnimation.Worlds(composite,plan.Original.Parents.Concat(attack.Bones.Select(b=>b.Parent<0?0:110+b.Parent)).ToArray());
            for(int i=0;i<97;i++) {int at=fm.At+i*64;var actual=p.Qs(at+16)*fullWorld[S(p,at)];var expected=sourceForward.Simple[i].Transform*sourceWorld[sourceForward.Simple[i].A];
                int body=source.BoneToBody[S(p,at+2)];foreach(var endpoint in new[]{source.Bodies[body].Capsule.A,source.Bodies[body].Capsule.B}){float e=Vector3.Distance(Vector3.Transform(endpoint,actual),Vector3.Transform(endpoint,expected));maxPose=Math.Max(maxPose,e);Check(e<.001f,"Animated source capsule placement differs");}}
        }
        return new{bodies=97,hinges=95,ragdollJoints=1,animationBones=356,ragdollBones=97,prefixUnmapped,sourceUnmapped,maxCapsule,maxPivot,maxMapper,maxPose};
    }
    static object ValidatePartConsumers(byte[] mapBytes,byte[] eventBytes,byte[] nativeParams,byte[] sourceParams,byte[] nativePwv) {
        var map=MSBB.Read(mapBytes);Check(map.Parts.Enemies.Single(e=>e.EntityID==2400207).ModelName=="c2730","Native part consumer model identity differs");
        PARAM Table(List<BinderFile> files) {var p=PARAM.Read(files.Single(f=>Leaf(f.Name).Equals("RagdollParam.param",StringComparison.OrdinalIgnoreCase)).Bytes);Check(p.DetectedSize==48&&p.ParamdefDataVersion==1,"RagdollParam schema changed");var def=new PARAMDEF{ParamType="RAGDOLL_PARAM_ST",DataVersion=1};def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=48});p.ApplyParamdef(def);return p;}
        byte[] Payload(PARAM p,int id)=>(byte[])p.Rows.Single(r=>r.ID==id).Cells.Single().Value;
        var source=Table(BND3.Read(sourceParams).Files);var native=Table(BND4.Read(nativeParams).Files);
        for(int slot=0;slot<7;slot++){var row=Payload(source,1400+slot);Check(row[33]==slot+1&&row.Where((_,i)=>i!=33).All(b=>b==0),"Hydra source RagdollParam gains/mask changed");}
        var pwv=HKXPWV.Read(nativePwv);Check(pwv.RagdollBoneEntries.Any(e=>e.RagdollParamID==273010&&e.NPCPartGroupIndex==7)&&pwv.RagdollBoneEntries.Any(e=>e.RagdollParamID==273011&&e.NPCPartGroupIndex==8),"Native PWV part/parameter relationship changed");
        Check(Payload(native,273010)[33]==10&&Payload(native,273011)[33]==11,"Native mask/parameter relationship changed");
        var events=EMEVD.Read(eventBytes);var zero=events.Events.Single(e=>e.ID==0);var resolved=new List<object>();
        foreach(var spec in new[]{(Slot:14,Part:2490,Group:7,Mask:10),(Slot:15,Part:2491,Group:8,Mask:11)})foreach(int eventId in new[]{12405430,12405460}) {
            var init=zero.Instructions.Single(i=>i.Bank==2000&&i.ID==0&&BitConverter.ToInt32(i.ArgData,0)==spec.Slot&&BitConverter.ToInt32(i.ArgData,4)==eventId);
            var template=events.Events.Single(e=>e.ID==eventId);var instructions=template.Instructions.Select(i=>new EMEVD.Instruction(i.Bank,i.ID,i.ArgData.ToArray())).ToArray();
            foreach(var q in template.Parameters)init.ArgData.AsSpan(checked(8+(int)q.SourceStartByte),checked((int)q.ByteCount)).CopyTo(instructions[q.InstructionIndex].ArgData.AsSpan(checked((int)q.TargetStartByte)));
            if(eventId==12405430){var part=instructions.Single(i=>i.Bank==2004&&i.ID==22).ArgData;Check(BitConverter.ToInt32(part)==2400207&&BitConverter.ToUInt16(part,4)==spec.Part&&BitConverter.ToUInt16(part,6)==spec.Group,"Native event part/PWV group relationship changed");}
            else {var masks=instructions.Where(i=>i.Bank==2004&&i.ID==27).Select(i=>i.ArgData).ToArray();Check(masks.Length==4&&masks.All(b=>BitConverter.ToInt32(b)==2400207)&&masks.Any(b=>b[4]==spec.Mask&&b[5]==1)&&masks.Any(b=>b[4]==spec.Mask&&b[5]==0),"Native event collision-mask consumer relationship changed");}
            resolved.Add(new{eventId,spec.Slot,spec.Part,spec.Group,spec.Mask});
        }
        return new{actor=2400207,model="c2730",sourceRagdollRows=Enumerable.Range(1400,7),nativeConsumers=resolved};
    }
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],output=args[2];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();
        byte[] Read(string root,string rel){string path=Path.Combine(root,rel);var bytes=File.ReadAllBytes(path);hashes.TryAdd(path,Hash(bytes));return bytes;}
        byte[]? Source(string rel)=>File.Exists(Path.Combine(sourceRoot,rel))?Read(sourceRoot,rel):null;
        var source=HydraPhysicsSource.Load(Source);var sourceBank=BND3.Read(Source("chr/c3530.chrbnd.dcx")!);
        var nativeBank=BND4.Read(Read(nativeRoot,"chr/c2730.chrbnd.dcx"));var nativeAnim=BND4.Read(Read(nativeRoot,"chr/c2730.anibnd.dcx"));
        var nativePhysics=nativeBank.Files.Single(f=>f.ID==300).Bytes.ToArray();var nativePwv=nativeBank.Files.Single(f=>f.Name.EndsWith(".hkxpwv")).Bytes.ToArray();
        var reference=SourceAnimation.Load(Source,"c3530",0);var attack=SourceAnimation.Load(Source,"c3530",3000);
        var plan=NonHumanoidEnemies.Rig(sourceBank.Files.Single(f=>Leaf(f.Name)=="c3530.flver").Bytes.ToArray(),nativeBank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray(),nativeAnim.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),reference);
        var hinge=BND4.Read(Read(nativeRoot,"chr/c1130.chrbnd.dcx")).Files.Single(f=>f.ID==300).Bytes.ToArray();
        var physics=HydraPhysicsRig.Convert(nativePhysics,source,plan);
        foreach(string name in new[]{"hknpCapsuleShape","hkpRagdollConstraintData","hkaSkeleton","hkaSkeletonMapper","hknpRagdollData"})Check(Signature(physics.Physics,name)==Signature(nativePhysics,name),"Native class signature differs: "+name);
        var checks=Validate(physics.Physics,source,plan,attack,hinge);
        var consumers=ValidatePartConsumers(Read(nativeRoot,"map/MapStudio/m24_00_00_00.msb.dcx"),Read(nativeRoot,"event/m24_00_00_00.emevd.dcx"),Read(nativeRoot,"param/gameparam/gameparam.parambnd.dcx"),Read(sourceRoot,"param/GameParam/GameParam.parambnd.dcx"),nativePwv);
        var ids=Enumerable.Range(1400,7).ToDictionary(id=>id,id=>883000+id-1400);var pwv=HydraPwv.Convert(nativePwv,source,plan,ids);
        var parsed=HKXPWV.Read(pwv.Pwv);Check(parsed.BoneMapEntries.Count==97&&parsed.AnimBoneEntries.Count==356&&parsed.RagdollBoneEntries.Count==97,"Native PWV arrays invalid");
        Check(parsed.BoneMapEntries.All(e=>e.UnknownValueA==-1&&e.UnknownValueB==0),"PWV signed widening invalid");
        Check(pwv.Pwv.AsSpan(32+97*8,110*8).SequenceEqual(nativePwv.AsSpan(32+60*8,110*8)),"Original native animation PWV prefix changed");
        Check(parsed.AnimBoneEntries.Skip(110).All(e=>e.SkeletonParamID==-1&&e.Unk2==0),"Source animation PWV entries changed");
        var byBody=source.Parts.SelectMany(part=>part.BodyIndices.Select(i=>(i,part))).ToDictionary(x=>x.i,x=>x.part);
        for(int i=0;i<97;i++){var e=parsed.RagdollBoneEntries[i];Check(e.DamageAnimID==-1&&e.Unknown==0&&e.UnknownBB==-1&&e.Unk0C==0&&e.Unk10==0,"Unsupported native ragdoll fields");
            if(byBody.TryGetValue(i,out var part))Check(e.RagdollParamID==ids[1400+part.Slot]&&e.NPCPartGroupIndex==part.Group,"Head hurtbox ownership/remap changed");else Check(e.RagdollParamID==-1&&e.NPCPartGroupIndex==0,"Unowned body acquired a head group");}
        Check(pwv.Report.RequiredSourceRagdollIDs.SequenceEqual(Enumerable.Range(1400,7))&&byBody.Count==28,"Required source row/head capsule closure differs");
        var corrupt=nativePhysics.ToArray();corrupt[16]=4;Reject(()=>HydraPhysicsRig.Convert(corrupt,source,plan),"native pointer ABI");
        var missing=ids.Where(p=>p.Key!=1406).ToDictionary(p=>p.Key,p=>p.Value);Reject(()=>HydraPwv.Convert(nativePwv,source,plan,missing),"missing owned parameter");
        var duplicate=new Dictionary<int,int>(ids){[1401]=ids[1400]};Reject(()=>HydraPwv.Convert(nativePwv,source,plan,duplicate),"duplicate owned parameters");
        var overlap=new Dictionary<int,int>(ids){[1400]=273000};Reject(()=>HydraPwv.Convert(nativePwv,source,plan,overlap),"native parameter collision");
        var badPwv=nativePwv.ToArray();badPwv[8]=1;Reject(()=>HydraPwv.Convert(badPwv,source,plan,ids),"wrong native PWV population");
        var badAdded=plan.Added.ToArray();badAdded[0]=badAdded[0] with{Name="Other"};Reject(()=>HydraPhysicsRig.Convert(nativePhysics,source,plan with{Added=badAdded}),"source bone ownership");
        var joint=source.Constraints.First(c=>c.Type=="hkpHingeConstraintData");Reject(()=>HydraPhysicsPacking.ConstraintBytes(joint with{FreeRotationAxis=3}),"unsupported hinge axis");
        foreach(var (path,hash) in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"Original input changed: "+path);
        File.WriteAllBytes(Path.Combine(output,"c3530-native.hkx"),physics.Physics);File.WriteAllBytes(Path.Combine(output,"c3530-native.hkxpwv"),pwv.Pwv);
        File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{checks,consumers,physics.Report,pwv=pwv.Report,rejected=7,inputs=hashes},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS Hydra native physics/PWV: 97 source capsules, 95 native plain hinges +1 ragdoll, 356/97 skeletons, both mappers, 28 head capsules; 7 invalid inputs rejected; {hashes.Count} actual input archives unchanged. No runtime/gameplay claim.");
    }
}

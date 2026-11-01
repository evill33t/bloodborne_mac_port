// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;
using K=HollowCombatProfiles.Kind;

static class HollowDeathMotionTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{a.M11-b.M11,a.M12-b.M12,a.M13-b.M13,a.M21-b.M21,a.M22-b.M22,a.M23-b.M23,a.M31-b.M31,a.M32-b.M32,a.M33-b.M33,a.M41-b.M41,a.M42-b.M42,a.M43-b.M43}.Max(Math.Abs);
    static Matrix4x4 Sample(Spline.TransformTrack t,float frame) {
        Vector3 Vector(bool scale) {
            var types=scale?t.Mask.ScaleTypes:t.Mask.PositionTypes;var spline=scale?t.SplineScale:t.SplinePosition;var result=scale?Vector3.One:Vector3.Zero;var stat=scale?t.StaticScale:t.StaticPosition;
            for(int i=0;i<3;i++){if(spline!=null)result[i]=(i==0?spline.GetValueX(frame):i==1?spline.GetValueY(frame):spline.GetValueZ(frame))??result[i];else if(types.Contains((Spline.FlagOffset)(1<<i)))result[i]=stat[i];}return result;
        }
        var q=t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.HasStaticRotation?t.StaticRotation:Quaternion.Identity;
        return Matrix4x4.CreateScale(Vector(true))*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(Vector(false));
    }
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],output=args[2];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();
        byte[] Read(string root,string rel){string path=Path.Combine(root,rel);var b=File.ReadAllBytes(path);hashes.TryAdd(path,Convert.ToHexStringLower(SHA256.HashData(b)));return b;}
        byte[]? Source(string rel)=>File.Exists(Path.Combine(sourceRoot,rel))?Read(sourceRoot,rel):null;
        var originalBytes=Read(nativeRoot,"chr/c2630.anibnd.dcx");var native=BND4.Read(Read(nativeRoot,"chr/c2630.chrbnd.dcx"));var original=BND4.Read(originalBytes);var reports=new List<object>();
        var roles=new (string Model,K Role,int Offset)[]{("c2500",K.Sword,0),("c2500",K.Torch,10000),("c2500",K.Bow,20000),("c2540",K.SwordShield,0),("c2540",K.Axe,10000),("c2540",K.Firebomb,0),("c2550",K.SpearShield,0),("c2550",K.SwordShield,10000),("c2550",K.Crossbow,20000),("c2550",K.CrossbowStationary,20000),("c2560",K.SwordShield,0),("c2560",K.RapierShield,20000)};
        int poses=0,clips=0;float worst=0;
        foreach(var role in roles) {
            var bank=BND4.Read(originalBytes);var motion=HollowCombatMotion.Create(Source,role.Model,role.Role,bank);
            var authored=MapCharacters.Authored(Source,role.Model);var reference=SourceAnimation.Load(Source,role.Model,0);
            int[] masks=(role.Model,role.Role) switch {
                ("c2500",K.Sword)=>[0],("c2500",K.Torch)=>[1],("c2500",K.Bow)=>[2],
                ("c2540",K.SwordShield)=>[0,2],("c2540",K.Axe)=>[1],("c2540",K.Firebomb)=>[0],
                ("c2550",K.SpearShield)=>[0,1],("c2550",K.SwordShield)=>[1,2],("c2550",_)=>[3],
                ("c2560",K.SwordShield)=>[0,3,7,10],("c2560",K.RapierShield)=>[2,4,7,10],_=>throw new Exception("Unexpected role")};
            var clothPhysics=SourceClothPose.ReadPhysics(Source,"DS1R",role.Model);var cloth=clothPhysics==null?null:SourceClothPose.Bake(authored.Model,clothPhysics);
            var prepared=HollowSourceSkin.PrepareModel(authored.Model,cloth);
            int[] dummies=role.Role==K.Bow?[30]:role.Role is K.Crossbow or K.CrossbowStationary?[20]:role.Role==K.Firebomb?[1]:[];
            var rig=HollowSourceSkin.Rig(prepared,native.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray(),bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),reference,rigidAnchors:cloth?.Anchors,masks:masks.ToHashSet(),sourceMuzzleIDs:dummies);
            var rigInfo=NativeAnimation.Skeleton(rig.Skeleton);
            var hidden=HollowSourceSkin.HiddenEquipmentBones(rig);var sourceBank=BND3.Read(Source("chr/"+role.Model+".anibnd.dcx")!);var sourceTae=TAE.Read(sourceBank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);var available=sourceTae.Animations.Select(a=>checked((int)a.ID)).ToHashSet();
            if(role.Role==K.Bow) {
                var selected=new[]{2002200,2002250,3002250}.Select(id=>motion.MotionSourceIDs[id]).ToArray();
                var expected=new[]{2200,2250,2250}.Select(id=>available.Contains(role.Offset+id)?role.Offset+id:id).ToArray();
                Check(selected.SequenceEqual(expected),"Dormant bow death selection changed");
                reports.Add(new{role.Model,Role=role.Role.ToString(),EncodingSupported=false,SourceActions=selected,Reason="Bow archer is explicitly omitted by current MapEnemySpawns; source selection only."});
                Console.WriteLine("PASS c2500/Bow selectedSourceIDs only; encoding unsupported/currently omitted");continue;
            }
            foreach(int id in new[]{2002200,2002250,3002250}) {
                var file=bank.Files.Single(f=>f.ID==1000000000+id);var source=motion.Clip(id);
                file.Bytes=NativeRigAppend.Clip(file.Bytes.ToArray(),rig,t=>HollowSourceSkin.Sample(rig,source,t*source.Duration),preserveAddedRootWorld:true,allowZeroAddedBones:hidden);
            }
            var result=HollowDeathMotion.Apply(bank,rig,motion);Check(result.Count==4&&bank.Files.Count==original.Files.Count+4,"Owned death membership changed");
            var expectedProject=original.Files.Single(f=>f.ID==7000300).Bytes.ToArray();
            foreach(var binding in result) {
                int baseID=binding.Action==2002211?2200:2250,expected=available.Contains(role.Offset+baseID)?role.Offset+baseID:baseID;
                Check(binding.SourceAction==expected,"Role death variant lost: "+role.Model+"/"+role.Role+" expected "+expected+" got "+binding.SourceAction);
                var file=bank.Files.Single(f=>f.ID==1000000000+binding.Action);var counterpart=bank.Files.Single(f=>f.ID==1000000000+binding.NativeCounterpart);
                Check(file.Bytes.Span.SequenceEqual(counterpart.Bytes.Span),"Owned death clip differs from its verified role counterpart");
                Encoding.ASCII.GetBytes("c2630").CopyTo(expectedProject,binding.ProjectOffset+@"Model\chr\".Length);
                var raw=file.Bytes.ToArray();var p=new NativeAnimation.Pack(raw);int a=p.Object("hkaSplineCompressedAnimation");var data=p.Array(a+152,1,64*1024*1024);
                var tracks=Spline.ReadSplineCompressedAnimByteBlock(false,raw.AsSpan(p.Data+data.At,data.Count).ToArray(),p.Int(a+20),p.Int(a+60));
                var source=motion.Clip(binding.NativeCounterpart);var sourceNames=source.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(x=>x.Name,x=>x.i);int last=p.Int(a+56)-1;
                foreach(int frame in new[]{0,last/2,last}.Distinct()) {
                    float fraction=frame/(float)last;int block=Math.Min(frame/255,tracks.Count-1);var local=tracks[block].Select(t=>Sample(t,frame-block*255)).ToArray();var parents=rigInfo.Parents;var world=NativeAnimation.Worlds(local,parents);
                    var expectedWorld=NativeAnimation.Worlds(source.Bones.Select((b,i)=>source.HollowLocal(i,fraction*source.Duration,true)).ToArray(),source.Bones.Select(b=>b.Parent).ToArray());
                    for(int i=84;i<rig.Count;i++){float error=Error(world[i],expectedWorld[sourceNames[rigInfo.Names[i][4..]]]);worst=Math.Max(worst,error);Check(error<.006f,"Death source frame mismatch: "+role.Model+"/"+role.Role+"/"+binding.Action);}
                    poses++;
                }
                clips++;
            }
            Check(expectedProject.SequenceEqual(bank.Files.Single(f=>f.ID==7000300).Bytes.ToArray()),"Changes escaped four project owner strings");
            foreach(var originalFile in original.Files.Where(f=>f.ID!=7000300&&f.ID is not(1002002200 or 1002002250 or 1003002250)))Check(originalFile.Bytes.Span.SequenceEqual(bank.Files.Single(f=>f.ID==originalFile.ID).Bytes.Span),"Unrelated native action/resource changed");
            reports.Add(new{role.Model,Role=role.Role.ToString(),EncodingSupported=true,Bindings=result});Console.WriteLine("PASS "+role.Model+"/"+role.Role+": "+string.Join(',',result.Select(b=>b.SourceAction)));
        }
        foreach(var(path,hash)in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==hash,"Input changed: "+path);
        File.WriteAllText(Path.Combine(output,"regression.json"),JsonSerializer.Serialize(new{status="PASS",clips,poses,worst,reports,hashes},new JsonSerializerOptions{WriteIndented=true}));Console.WriteLine($"PASS 12role selections/11supported encodings/{clips}owneddeathclips/{poses}completeposes/{hashes.Count}unchangedinputs; bow encoding omitted; nativegameplaypending");
    }
}

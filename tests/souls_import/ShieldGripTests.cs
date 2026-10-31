// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class ShieldGripTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Reject(Action call,string message){try{call();}catch(InvalidDataException){return;}throw new Exception(message);}
    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static string Key(string name)=>new(name.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
    static Matrix4x4 Inv(Matrix4x4 value){Check(Matrix4x4.Invert(value,out var result),"Singular fixture skin matrix");return result;}
    static Matrix4x4 Matrix(Vector3 p,Quaternion q,Vector3 s)=>Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(p);
    static Matrix4x4 At(Spline.TransformTrack t,float frame) {
        var p=t.StaticPosition;var s=t.StaticScale;
        if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(frame)??p.X,t.SplinePosition.GetValueY(frame)??p.Y,t.SplinePosition.GetValueZ(frame)??p.Z);
        if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(frame)??s.X,t.SplineScale.GetValueY(frame)??s.Y,t.SplineScale.GetValueZ(frame)??s.Z);
        return Matrix(p,t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.StaticRotation,s);
    }
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{a.M11-b.M11,a.M12-b.M12,a.M13-b.M13,a.M21-b.M21,a.M22-b.M22,a.M23-b.M23,a.M31-b.M31,a.M32-b.M32,a.M33-b.M33,a.M41-b.M41,a.M42-b.M42,a.M43-b.M43}.Max(Math.Abs);
    static List<Spline.TransformTrack[]> Decode(byte[] bytes) {
        var p=new NativeAnimation.Pack(bytes);int a=p.Object("hkaSplineCompressedAnimation");var data=p.Array(a+152,1,64*1024*1024);
        return Spline.ReadSplineCompressedAnimByteBlock(false,bytes.AsSpan(p.Data+data.At,data.Count).ToArray(),p.Int(a+20),p.Int(a+60));
    }
    public static void Main(string[] args) {
        string source=args[0],mod=args[1],output=args[2],native=args[3];Directory.CreateDirectory(output);var hashes=new Dictionary<string,byte[]>();
        byte[] Read(string path){var value=File.ReadAllBytes(path);hashes.TryAdd(path,SHA256.HashData(value));return value;}
        byte[]? Source(string path)=>Read(Path.Combine(source,path));
        var retailBank=BND4.Read(Read(Path.Combine(native,"chr/c2610.anibnd.dcx")));
        var retailSkeleton=retailBank.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        var retailModel=BND4.Read(Read(Path.Combine(native,"chr/c2610.chrbnd.dcx"))).Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();
        var retailClips=retailBank.Files.Where(f=>f.ID>=1000000000).ToDictionary(f=>f.ID,f=>f.Bytes.ToArray());
        using var conversion=JsonDocument.Parse(Read(Path.Combine(mod,"maps/m10_01_00_00/conversion.json")));
        var spawns=conversion.RootElement.GetProperty("enemySpawns").GetProperty("Spawns").EnumerateArray()
            .Where(s=>new[]{"c2540","c2550","c2560"}.Contains(s.GetProperty("SourceModel").GetString()))
            .GroupBy(s=>s.GetProperty("TargetModel").GetString()).Select(g=>g.First()).ToArray();
        var reports=new List<object>();int clipsChecked=0,posesChecked=0,additives=0,contactModels=0;
        foreach(var spawn in spawns) {
            string model=spawn.GetProperty("SourceModel").GetString()!,target=spawn.GetProperty("TargetModel").GetString()!;
            var masks=spawn.GetProperty("DisplayMasks").EnumerateArray().Select(v=>v.GetInt32()).ToHashSet();
            var sb=BND3.Read(Source("chr/"+model+".chrbnd.dcx")!);var sourceBytes=sb.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();var src=FLVER2.Read(sourceBytes);
            var mounts=SourceEquipmentMounts.Load(Source,"DS1R",model,sourceBytes,masks)!;
            if(mounts.Grip.Count==0){Check(!mounts.Report.Mounts.Any(m=>Key(m.Bone)=="lshield"),"Shield loadout did not receive a holding pose");continue;}
            Check(mounts.Grip.Count==7&&mounts.Report.HoldingPoseBones.Count==7,"Holding report lost source provenance");
            var targetBnd=BND4.Read(Read(Path.Combine(mod,"dvdroot_ps4/chr/"+target+".chrbnd.dcx")));var tf=FLVER2.Read(targetBnd.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);var tb=SkinRetarget.World(tf.Nodes);
            var bank=BND4.Read(Read(Path.Combine(mod,"dvdroot_ps4/chr/"+target+".anibnd.dcx")));var rig=NativeAnimation.Skeleton(bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
            var proportions=EnemyProportions.Create(sourceBytes,retailModel,retailSkeleton);
            Check(bank.Files.Single(f=>f.ID==1000000).Bytes.Span.SequenceEqual(proportions.Skeleton),"Actual shield rig differs from the source-proportioned native skeleton");
            var gripIndices=mounts.Grip.Keys.Select(n=>Array.IndexOf(rig.Names,n)).ToHashSet();
            var original=bank.Files.Where(f=>f.ID>=1000000000).ToArray();Check(original.Length==328,"Fixture must cover every native c2610 clip");
            var unchanged=bank.Files.Where(f=>f.ID<1000000000).ToDictionary(f=>f.ID,f=>(f.Name,f.Flags,Hash:SHA256.HashData(f.Bytes.Span)));
            var sw=SkinRetarget.World(src.Nodes);var sourceClip=SourceAnimation.Load(Source,model,0);var sourceWorld=NativeAnimation.Worlds(sourceClip.Sample(0),sourceClip.Bones.Select(b=>b.Parent).ToArray());var sourceRig=sourceClip.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(v=>v.Name,v=>v.i);
            var selected=src.Meshes.Where(m=>{var mask=System.Text.RegularExpressions.Regex.Match(src.Materials[m.MaterialIndex].Name,@"#(\d+)#");return !mask.Success||masks.Contains(int.Parse(mask.Groups[1].Value));}).ToArray();
            var shields=new List<(Vector3 source,Vector3 target,int node)>();var hands=new List<(Vector3 source,Vector3 target,int node)>();
            for(int m=0;m<selected.Length;m++)for(int v=0;v<selected[m].Vertices.Count;v++) {
                var sv=selected[m].Vertices[v];if(sv.BoneWeights[0]<.9999f)continue;int b=SkinRetarget.Bone(selected[m],sv.BoneIndices[0]);var name=src.Nodes[b].Name;if(Key(name)!="lshield"&&name!="L Hand")continue;
                var tv=tf.Meshes[m].Vertices[v];Check(tv.BoneWeights[0]==1,"Fixture grip needs rigid shield/hand points");int t=SkinRetarget.Bone(tf.Meshes[m],tv.BoneIndices[0]);
                var p=Vector3.Transform(sv.Position,Inv(sw[b])*sourceWorld[sourceRig[name]]);
                (Key(name)=="lshield"?shields:hands).Add((p,tv.Position,t));
            }
            Check(shields.Count>200&&hands.Count>0,"Actual shield/gauntlet geometry fixture is incomplete");
            var pair=shields.Select(s=>(s,h:hands.MinBy(h=>Vector3.DistanceSquared(s.source,h.source)))).MinBy(v=>Vector3.DistanceSquared(v.s.source,v.h.source));
            float authoredDistance=Vector3.Distance(pair.s.source,pair.h.source),worstError=0,oldError=0;int actorPoses=0,originalInputs=0,correctedInputs=0;
            Check(authoredDistance<.05f,"Source contact fixture does not touch the handle");
            foreach(var file in original) {
                // Derive the former torch grip from retail, even when this
                // fixture is rerun against an already-corrected production mod.
                byte[] before=EnemyProportions.Clip(retailClips[file.ID],proportions),after=SourceEquipmentMounts.ApplyGrip(before,rig,mounts.Grip);
                if(file.Bytes.Span.SequenceEqual(after))correctedInputs++;
                else {Check(file.Bytes.Span.SequenceEqual(before),"Actual shield clip matches neither the retail-proportioned nor corrected policy: "+target+"/"+file.ID);originalInputs++;}
                var p=new NativeAnimation.Pack(before);var q=new NativeAnimation.Pack(after);int a=p.Object("hkaSplineCompressedAnimation");int blend=before[p.Data+p.Object("hkaAnimationBinding")+80];
                // Appending replacement arrays must preserve all original data
                // objects (including extracted motion, annotations and timing),
                // except the three explicitly relocated array descriptors.
                var body=after.AsSpan(q.Data,p.Size).ToArray();foreach(int descriptor in new[]{a+88,a+104,a+152})before.AsSpan(p.Data+descriptor,16).CopyTo(body.AsSpan(descriptor,16));
                Check(body.AsSpan().SequenceEqual(before.AsSpan(p.Data,p.Size)),"Holding pose changed native non-array data objects");
                var old=Decode(before);var changed=Decode(after);var twice=Decode(SourceEquipmentMounts.ApplyGrip(after,rig,mounts.Grip));Check(old.Count==changed.Count&&changed.Count==twice.Count,"Holding pose changed block count");
                for(int block=0;block<old.Count;block++)foreach(float frame in new[]{0f,Math.Min(255,p.Int(a+56)-1-block*255)*.5f,(float)Math.Min(255,p.Int(a+56)-1-block*255)}) {
                    var oldLocal=new Matrix4x4[rig.Names.Length];var newLocal=new Matrix4x4[rig.Names.Length];
                    for(int bone=0;bone<rig.Names.Length;bone++) {
                        oldLocal[bone]=At(old[block][bone],frame);newLocal[bone]=At(changed[block][bone],frame);
                        Check(newLocal[bone]==At(twice[block][bone],frame),"Repeated grip adaptation changed the serialized pose");
                        if(!gripIndices.Contains(bone))Check(oldLocal[bone]==newLocal[bone],"Holding pose changed native body/root/forearm tracks: "+file.ID+"/"+rig.Names[bone]);
                        else Check(Error(newLocal[bone],blend==1?Matrix4x4.Identity:mounts.Grip[rig.Names[bone]])<.001f,"Source wrist/finger pose did not survive native serialization");
                    }
                    posesChecked++;actorPoses++;
                    if(blend==0) {
                        var oldWorld=NativeAnimation.Worlds(oldLocal,rig.Parents);var newWorld=NativeAnimation.Worlds(newLocal,rig.Parents);
                        Vector3 Skin((Vector3 source,Vector3 target,int node) v,Matrix4x4[] world)=>Vector3.Transform(v.target,Inv(tb[v.node])*world[Array.IndexOf(rig.Names,tf.Nodes[v.node].Name)]);
                        float beforeDistance=Vector3.Distance(Skin(pair.s,oldWorld),Skin(pair.h,oldWorld));float afterDistance=Vector3.Distance(Skin(pair.s,newWorld),Skin(pair.h,newWorld));
                        oldError=Math.Max(oldError,Math.Abs(beforeDistance-authoredDistance));worstError=Math.Max(worstError,Math.Abs(afterDistance-authoredDistance));
                        Check(Math.Abs(afterDistance-authoredDistance)<.001f,"Source shield/hand contact drifted during native motion: "+target+"/"+file.ID+" error "+Math.Abs(afterDistance-authoredDistance));
                        Check(Skin(pair.s,oldWorld)==Skin(pair.s,newWorld),"Holding adaptation moved the forearm-strapped shield");
                    }
                }
                if(blend==1)additives++;clipsChecked++;file.Bytes=after;
            }
            Check(oldError>.03f,"Actual native animation fixture does not expose separated source handle/grip");contactModels++;
            if(target is "c9616" or "c9617") {
                string path=Path.Combine(output,"candidate/dvdroot_ps4/chr/"+target+".anibnd.dcx");Directory.CreateDirectory(Path.GetDirectoryName(path)!);File.WriteAllBytes(path,bank.Write(DCX.Type.DCX_DFLT_10000_44_9));var check=BND4.Read(path);Check(check.Files.Count==bank.Files.Count,"Candidate binder changed member population");
                foreach(var file in check.Files.Where(f=>f.ID<1000000000)){var old=unchanged[file.ID];Check(file.Name==old.Name&&file.Flags==old.Flags&&SHA256.HashData(file.Bytes.Span).AsSpan().SequenceEqual(old.Hash),"Candidate changed native skeleton/TAE/behavior or member identity");}
            }
            Check(originalInputs==328||correctedInputs==328,"A shield bank mixes pre-grip and corrected policies");
            reports.Add(new{model,target,masks,inputPolicy=correctedInputs==328?"corrected":"original",sourceContact=authoredDistance,previousWorstContactError=oldError,correctedWorstContactError=worstError,actorPoses,holdingBones=mounts.Grip.Keys});
            Reject(()=>SourceEquipmentMounts.ApplyGrip(original[0].Bytes.ToArray(),rig,mounts.Grip.Where(v=>v.Key!="L Hand").ToDictionary()),"Incomplete grip accepted");
            var invalid=mounts.Grip.ToDictionary();invalid["L Hand"]=Matrix4x4.CreateScale(2);Reject(()=>SourceEquipmentMounts.ApplyGrip(original[0].Bytes.ToArray(),rig,invalid),"Nonrigid grip accepted");
            Check(SourceEquipmentMounts.ApplyGrip(original[0].Bytes.ToArray(),rig,new Dictionary<string,Matrix4x4>()).SequenceEqual(original[0].Bytes.ToArray()),"Empty grip changed no-shield animation");
            Console.WriteLine($"PASS {target}: all328 clips, source handle contact {authoredDistance*1000:F2}mm, corrected max error {worstError*1000:F3}mm (was {oldError*1000:F1}mm)");
        }
        foreach(var (path,hash) in hashes)Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash),"Original input changed: "+path);
        Check(contactModels==7&&clipsChecked==7*328&&additives>0,"Installed animated shield coverage changed");
        File.WriteAllText(Path.Combine(output,"grip-regression.json"),JsonSerializer.Serialize(new{status="PASS",contactModels,clipsChecked,posesChecked,additives,hashes=hashes.ToDictionary(p=>p.Key,p=>Convert.ToHexStringLower(p.Value)),reports},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS all{clipsChecked} clips/{posesChecked} poses, {additives} additive clips, {hashes.Count} unchanged source/native inputs; native gameplay pending");
    }
}

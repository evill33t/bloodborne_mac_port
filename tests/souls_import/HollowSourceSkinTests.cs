// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class HollowSourceSkinTests {
    static void Check(bool ok,string why){if(!ok)throw new Exception(why);}
    static void Reject(Action call,string why){try{call();}catch(InvalidDataException){return;}throw new Exception(why);}
    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static string Key(string name)=>new(name.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
    static bool Equipment(string name)=>System.Text.RegularExpressions.Regex.IsMatch(Key(name),@"\A(?:rweapon\d*|lweapon\d*|lshield|sword|bpw00)\z");
    static Matrix4x4 Inv(Matrix4x4 m){Check(Matrix4x4.Invert(m,out var v),"Singular test bind");return v;}
    static Matrix4x4 Matrix(Vector3 p,Quaternion q,Vector3 s)=>Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(p);
    static Matrix4x4 Reference(SourceAnimation.Bone b)=>Matrix(b.Translation,b.Rotation,b.Scale);
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{a.M11-b.M11,a.M12-b.M12,a.M13-b.M13,a.M21-b.M21,a.M22-b.M22,a.M23-b.M23,a.M31-b.M31,a.M32-b.M32,a.M33-b.M33,a.M41-b.M41,a.M42-b.M42,a.M43-b.M43}.Max(Math.Abs);
    static Matrix4x4 At(Spline.TransformTrack track,float frame,SourceAnimation.Bone? fallback=null) {
        Vector3 Vector(bool scale) {
            var types=scale?track.Mask.ScaleTypes:track.Mask.PositionTypes;var spline=scale?track.SplineScale:track.SplinePosition;
            var value=scale?(fallback?.Scale??Vector3.One):(fallback?.Translation??Vector3.Zero);var stat=scale?track.StaticScale:track.StaticPosition;
            for(int axis=0;axis<3;axis++) {
                if(spline!=null)value[axis]=(axis==0?spline.GetValueX(frame):axis==1?spline.GetValueY(frame):spline.GetValueZ(frame))??value[axis];
                else if(types.Contains((Spline.FlagOffset)(1<<axis)))value[axis]=stat[axis];
            }
            return value;
        }
        return Matrix(Vector(false),track.HasSplineRotation?track.SplineRotation.GetValue(frame):track.HasStaticRotation?track.StaticRotation:fallback?.Rotation??Quaternion.Identity,Vector(true));
    }
    sealed class IndependentSource {
        readonly List<Spline.TransformTrack[]> blocks;readonly int[] bindings;readonly SourceAnimation info;
        internal IndependentSource(byte[] bytes,SourceAnimation metadata) {
            info=metadata;var tag=new SourceCollision.Tag(bytes);var anim=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkaSplineCompressedAnimation");var binding=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkaAnimationBinding");
            var data=tag.Array(anim,"data");var map=tag.Array(binding,"transformTrackToBoneIndices");bindings=Enumerable.Range(0,map.Count).Select(i=>(int)unchecked((short)tag.UShort(tag.Element(map,i).At))).ToArray();
            blocks=Spline.ReadSplineCompressedAnimByteBlock(false,Enumerable.Range(data.At,data.Count).Select(tag.Byte).ToArray(),bindings.Length,tag.Int(tag.Nested(anim.Type,anim.At,"numBlocks").At));
        }
        internal Matrix4x4[] Sample(float fraction) {
            float frame=(info.FrameCount-1)*fraction;int block=Math.Min((int)(frame/255),blocks.Count-1);var values=info.Bones.Select(Reference).ToArray();
            for(int i=0;i<bindings.Length;i++)values[bindings[i]]=At(blocks[block][i],frame-block*255,info.Bones[bindings[i]]);return values;
        }
    }
    static List<Spline.TransformTrack[]> NativeTracks(byte[] bytes) {
        var p=new NativeAnimation.Pack(bytes);int a=p.Object("hkaSplineCompressedAnimation");var data=p.Array(a+152,1,64*1024*1024);
        return Spline.ReadSplineCompressedAnimByteBlock(false,bytes.AsSpan(p.Data+data.At,data.Count).ToArray(),p.Int(a+20),p.Int(a+60));
    }
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],output=args[2];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();
        byte[] Read(string root,string path){string full=Path.Combine(root,path);var b=File.ReadAllBytes(full);hashes.TryAdd(full,Convert.ToHexStringLower(SHA256.HashData(b)));return b;}
        var nativeBank=BND4.Read(Read(nativeRoot,"chr/c2630.anibnd.dcx"));var nativeModel=BND4.Read(Read(nativeRoot,"chr/c2630.chrbnd.dcx")).Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();var nativeSkeleton=nativeBank.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        var nativeVisual=FLVER2.Read(nativeModel);var nativeThrow=nativeVisual.Dummies.Single(d=>d.ReferenceID==1);
        Check(nativeVisual.Nodes[nativeThrow.ParentBoneIndex].Name=="Model_Dmy"&&nativeVisual.Nodes[nativeThrow.AttachBoneIndex].Name=="R Hand"&&nativeThrow.FollowAttachBone&&nativeThrow.UseUpwardVector,"Native firebomb socket contract changed");
        var nativeRig=NativeAnimation.Skeleton(nativeSkeleton);Check(nativeRig.Names.Length==84,"c2630 fixture rig changed");
        var oldBank=BND4.Read(Read(nativeRoot,"chr/c2610.anibnd.dcx"));var oldModel=BND4.Read(Read(nativeRoot,"chr/c2610.chrbnd.dcx")).Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();var oldSkeleton=oldBank.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        byte[] nativeIdle=nativeBank.Files.Single(f=>f.ID==1000000000).Bytes.ToArray();
        var cases=new Dictionary<string,int[][]>{{"c2500",[[0],[1]]},{"c2540",[[0],[0,2],[1]]},{"c2550",[[0,1],[1,2],[3]]},{"c2560",[[2,4,7,10],[0,3,7,10]]}};
        var reports=new List<object>();int sourcePoses=0,nativePoses=0,verticesChecked=0,emitted=0,hiddenFrames=0,fullFrames=0;float worstSourceError=0,worstNativeError=0;bool sawAnimatedShield=false;
        foreach(var (model,variants) in cases) {
            byte[] archiveBytes=Read(sourceRoot,"chr/"+model+".anibnd.dcx");var archive=BND3.Read(archiveBytes);byte[] skeleton=archive.Files.Single(f=>Leaf(f.Name).Equals("skeleton.hkx",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
            var sourceBundle=BND3.Read(Read(sourceRoot,"chr/"+model+".chrbnd.dcx"));byte[] original=sourceBundle.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();
            var reference=SourceAnimation.Decode(skeleton,archive.Files.Single(f=>Leaf(f.Name)=="a00_0000.hkx").Bytes.ToArray());SourceClothPose.Result? cloth=model=="c2560"?SourceClothPose.Bake(original,sourceBundle.Files.Single(f=>Leaf(f.Name)=="c2560_c.hkx").Bytes.ToArray()):null;
            if(model=="c2540") {
                var tae=TAE.Read(archive.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);var throws=tae.Animations.Single(a=>a.ID==3008).Events.Where(e=>e.Type==2).ToArray();
                Check(throws.Length==1&&throws[0].StartTime==1.4666667f&&throws[0].EndTime==1.5f&&Convert.ToHexString(throws[0].GetParameterBytes(false))=="0100000000000000B400000000000000","Source firebomb throw/dummy contract changed");
            }
            byte[] prepared=HollowSourceSkin.PrepareModel(original,cloth);var src=FLVER2.Read(prepared);var sourceBind=SkinRetarget.World(src.Nodes);var boneNames=reference.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(v=>v.Name,v=>v.i,StringComparer.Ordinal);
            Check(reference.Bones[0].Name=="Master"&&reference.Bones.Single(b=>b.Name=="Pelvis").Parent==0&&src.Nodes.Single(n=>n.Name=="Pelvis").ParentIndex==-1,"Actual Master/Pelvis source contract");
            if(cloth!=null) {
                var settled=FLVER2.Read(cloth.Model);var restored=FLVER2.Read(prepared);
                foreach(int index in cloth.Anchors.Keys) {
                    var a=restored.Meshes[index];var b=settled.Meshes[index];Check(a.Vertices.Count==b.Vertices.Count,"Cape population changed");
                    foreach(var (v,i) in a.Vertices.Select((v,i)=>(v,i))){var old=b.Vertices[i];Check(v.Position==old.Position&&v.Normal==old.Normal&&v.Tangents.SequenceEqual(old.Tangents)&&v.UVs.SequenceEqual(old.UVs)&&v.Colors.SequenceEqual(old.Colors),"Cape reweight moved geometry/attributes");}
                    restored.Meshes[index]=b;
                }
                Check(restored.Write().SequenceEqual(settled.Write()),"Cape preparation changed unrelated data");
                Reject(()=>HollowSourceSkin.Rig(cloth.Model,nativeModel,nativeSkeleton,reference,cloth.Anchors),"Unprepared static cape accepted");
            } else Check(prepared.SequenceEqual(original),"No-cloth source model changed");
            foreach(var masksArray in variants) {
                var masks=masksArray.ToHashSet();int[] muzzle=model=="c2500"&&masks.Contains(0)?[30]:model=="c2550"&&masks.Contains(3)?[20]:model=="c2540"&&masks.SetEquals(new[]{0})?[1]:[];
                var plan=HollowSourceSkin.Rig(prepared,nativeModel,nativeSkeleton,reference,cloth?.Anchors,masks,muzzle);var rig=NativeAnimation.Skeleton(plan.Skeleton);var visibility=HollowSourceSkin.HiddenEquipmentBones(plan);var targetBind=SkinRetarget.World(FLVER2.Read(plan.Model).Nodes);
                var sockets=new List<(FLVER.Dummy Source,FLVER.Dummy Native)>();
                if(masksArray==variants[0])Check(HollowSourceSkin.Rig(prepared,oldModel,oldSkeleton,reference,cloth?.Anchors,masks,muzzle).Original.Names.Length==80,"Original80-bone donor path changed");
                foreach(int id in muzzle) {
                    var authored=src.Dummies.Single(d=>d.ReferenceID==id);var rebound=HollowSourceSkin.RebindDummy(authored,prepared,plan,id==1?(short)1:(short)60);sockets.Add((authored,rebound));var target=FLVER2.Read(plan.Model);var world=SkinRetarget.World(target.Nodes);var before=authored.ParentBoneIndex<0?Matrix4x4.Identity:sourceBind[authored.ParentBoneIndex];
                    Check(plan.Added.All(b=>b.Name!="DS1_Model_Dmy"),"Unanimated authoring helper was invented as an animation bone");
                    Check(Vector3.Distance(Vector3.Transform(authored.Position,before),Vector3.Transform(rebound.Position,world[rebound.ParentBoneIndex]))<.00001f,"Projectile rest position changed");
                    Check(Vector3.Distance(Vector3.TransformNormal(authored.Forward,before),Vector3.TransformNormal(rebound.Forward,world[rebound.ParentBoneIndex]))<.00001f,"Projectile rest direction changed");
                    Check(rebound.FollowAttachBone==authored.FollowAttachBone&&rebound.UseUpwardVector==authored.UseUpwardVector&&rebound.Color==authored.Color&&rebound.Unk30==authored.Unk30&&rebound.Unk34==authored.Unk34,"Projectile dummy metadata changed");
                    if(authored.AttachBoneIndex>=0)Check(target.Nodes[rebound.AttachBoneIndex].Name=="DS1_"+src.Nodes[authored.AttachBoneIndex].Name,"Animated projectile attachment not retained");
                    if(id==1) {
                        var bad=new FLVER.Dummy(authored){AttachBoneIndex=0};Reject(()=>HollowSourceSkin.RebindDummy(bad,prepared,plan,1),"Changed firebomb attachment accepted");
                        var badModel=FLVER2.Read(prepared);badModel.Nodes.Single(n=>n.Name=="c2540_weapon").Name="other_character";Reject(()=>HollowSourceSkin.Rig(badModel.Write(),nativeModel,nativeSkeleton,reference,masks:masks,sourceMuzzleIDs:new[]{1}),"Unrelated dummy1 accepted");
                    }
                }
                Check(rig.Names.Take(84).SequenceEqual(nativeRig.Names)&&rig.Local.Take(84).SequenceEqual(nativeRig.Local)&&rig.Parents.Take(84).SequenceEqual(nativeRig.Parents),"Native controller bones changed");
                foreach(var b in plan.Added){string name=b.Name[4..];Check(Error(sourceBind[src.Nodes.FindIndex(n=>n.Name==name)],targetBind[Array.IndexOf(rig.Names,b.Name)])<1e-6f,"Named source bind frame changed");}
                bool Selected(FLVER2.Mesh m){var match=System.Text.RegularExpressions.Regex.Match(src.Materials[m.MaterialIndex].Name,@"#(\d+)#");return !match.Success||masks.Contains(int.Parse(match.Groups[1].Value));}
                var selected=src.Meshes.Where(Selected).ToArray();var textures=src.Materials.SelectMany(m=>m.Textures).Where(t=>t.ParamName=="g_Diffuse").Select(t=>Leaf(t.Path).Split('.')[0]).Distinct().Select(n=>new TPF.Texture(n,9,0,SkinRetarget.RgbaTexture([255,255,255,255]),TPF.TPFPlatform.PC)).ToArray();
                var bake=SkinRetarget.Bake(prepared,plan.Model,textures,masks,overrides:HollowSourceSkin.Overrides(plan),sourceProfile:"DS1R");var rendered=FLVER2.Read(bake.Model.Write());var renderedBind=SkinRetarget.World(rendered.Nodes);var renderToRig=rendered.Nodes.Select(n=>Array.IndexOf(rig.Names,n.Name)).ToArray();
                for(int m=0;m<selected.Length;m++)for(int v=0;v<selected[m].Vertices.Count;v++)Check(Vector3.Distance(selected[m].Vertices[v].Position,rendered.Meshes[m].Vertices[v].Position)<.00001f,"Full source skin bind geometry changed");
                int clips=0,poses=0;float shieldMotion=0;Matrix4x4? shieldFirst=null;
                foreach(var file in archive.Files.Where(f=>System.Text.RegularExpressions.Regex.IsMatch(Leaf(f.Name),@"^a00_(?:0000|020[0-3]|300\d|3200|3210|3220)\.hkx$"))) {
                    var clip=SourceAnimation.Decode(skeleton,file.Bytes.ToArray());var independent=new IndependentSource(file.Bytes.ToArray(),clip);clips++;
                    byte[] appended=NativeRigAppend.Clip(nativeIdle,plan,t=>HollowSourceSkin.Sample(plan,clip,t*clip.Duration),preserveAddedRootWorld:true,allowZeroAddedBones:visibility);var packed=new NativeAnimation.Pack(appended);int animation=packed.Object("hkaSplineCompressedAnimation");var tracks=NativeTracks(appended);emitted++;
                    var originalPack=new NativeAnimation.Pack(nativeIdle);int originalAnim=originalPack.Object("hkaSplineCompressedAnimation");var originalData=originalPack.Array(originalAnim+152,1,64*1024*1024);var newData=packed.Array(animation+152,1,64*1024*1024);int oldUsed=originalPack.Int(originalPack.Array(originalAnim+104,4,256).At);
                    Check(appended.AsSpan(packed.Data+newData.At,84*4).SequenceEqual(nativeIdle.AsSpan(originalPack.Data+originalData.At,84*4))&&appended.AsSpan(packed.Data+newData.At+plan.Count*4,oldUsed-84*4).SequenceEqual(nativeIdle.AsSpan(originalPack.Data+originalData.At+84*4,oldUsed-84*4)),"Native controller compressed channels changed");
                    var originalBody=appended.AsSpan(packed.Data,originalPack.Size).ToArray();int binding=originalPack.Object("hkaAnimationBinding");
                    foreach(int field in new[]{originalAnim+20,originalAnim+68})nativeIdle.AsSpan(originalPack.Data+field,4).CopyTo(originalBody.AsSpan(field,4));
                    foreach(int field in new[]{originalAnim+88,originalAnim+104,originalAnim+152,binding+32})nativeIdle.AsSpan(originalPack.Data+field,16).CopyTo(originalBody.AsSpan(field,16));
                    Check(originalBody.AsSpan().SequenceEqual(nativeIdle.AsSpan(originalPack.Data,originalPack.Size)),"Native animation objects/timing/extracted motion changed");
                    foreach(float fraction in new[]{0f,.5f,1f}) {
                        var expectedLocal=independent.Sample(fraction);var sourceWorld=NativeAnimation.Worlds(expectedLocal,clip.Bones.Select(b=>b.Parent).ToArray());var actualLocal=HollowSourceSkin.Sample(plan,clip,clip.Duration*fraction);
                        for(int i=0;i<plan.Added.Count;i++) {
                            string name=plan.Added[i].Name[4..];var expected=expectedLocal[boneNames[name]];if(i==0)expected*=Inv(plan.Original.Local[0]);float error=Error(actualLocal[i],expected);worstSourceError=Math.Max(worstSourceError,error);Check(error<.00001f,"Independent source equipment/body frame mismatch: "+model+"/"+Leaf(file.Name)+"/"+name);
                            if(Equipment(name)){if(Math.Abs(actualLocal[i].GetDeterminant())<1e-15f)hiddenFrames++;if(Key(name)=="lshield"){shieldFirst??=actualLocal[i];shieldMotion=Math.Max(shieldMotion,Error(shieldFirst.Value,actualLocal[i]));}}
                        }
                        float frame=(packed.Int(animation+56)-1)*fraction;int block=Math.Min((int)(frame/255),tracks.Count-1);var targetWorld=NativeAnimation.Worlds(tracks[block].Select(t=>At(t,frame-block*255)).ToArray(),rig.Parents);
                        foreach(int i in Enumerable.Range(0,plan.Added.Count)) {
                            string name=plan.Added[i].Name[4..];float error=Error(targetWorld[84+i],sourceWorld[boneNames[name]]);worstNativeError=Math.Max(worstNativeError,error);Check(error<.006f,"Native encoded full source frame mismatch: "+model+"/"+Leaf(file.Name)+"/"+name+" "+error);
                        }
                        foreach(var socket in sockets) {
                            var s=socket.Source;var n=socket.Native;var before=s.ParentBoneIndex<0?Matrix4x4.Identity:sourceBind[s.ParentBoneIndex];var after=targetBind[n.ParentBoneIndex];
                            if(s.FollowAttachBone&&s.AttachBoneIndex>=0){before*=Inv(sourceBind[s.AttachBoneIndex])*sourceWorld[boneNames[src.Nodes[s.AttachBoneIndex].Name]];after*=Inv(targetBind[n.AttachBoneIndex])*targetWorld[n.AttachBoneIndex];}
                            Check(Vector3.Distance(Vector3.Transform(s.Position,before),Vector3.Transform(n.Position,after))<.008f&&Vector3.Distance(Vector3.TransformNormal(s.Forward,before),Vector3.TransformNormal(n.Forward,after))<.008f&&Vector3.Distance(Vector3.TransformNormal(s.Upward,before),Vector3.TransformNormal(n.Upward,after))<.008f,"Animated source projectile frame changed: "+model+"/"+Leaf(file.Name));
                        }
                        for(int m=0;m<selected.Length;m++)for(int v=0;v<selected[m].Vertices.Count;v++) {
                            var sv=selected[m].Vertices[v];var tv=rendered.Meshes[m].Vertices[v];Vector3 expected=Vector3.Zero,actual=Vector3.Zero;
                            for(int slot=0;slot<4;slot++) {
                                if(sv.BoneWeights[slot]>0){int b=SkinRetarget.Bone(selected[m],sv.BoneIndices[slot]);expected+=Vector3.Transform(sv.Position,Inv(sourceBind[b])*sourceWorld[boneNames[src.Nodes[b].Name]])*sv.BoneWeights[slot];}
                                if(tv.BoneWeights[slot]>0){int b=SkinRetarget.Bone(rendered.Meshes[m],tv.BoneIndices[slot]);actual+=Vector3.Transform(tv.Position,Inv(renderedBind[b])*targetWorld[renderToRig[b]])*tv.BoneWeights[slot];}
                            }
                            Check(Vector3.Distance(expected,actual)<.008f,"Full visible vertex/weapon frame diverged: "+model+"/"+Leaf(file.Name));verticesChecked++;
                        }
                        sourcePoses++;nativePoses++;poses++;fullFrames+=plan.Added.Count;
                    }
                }
                if(shieldMotion>.05f)sawAnimatedShield=true;
                Check(clips>=10,"Insufficient source attack/transition fixture coverage");
                Reject(()=>NativeRigAppend.Clip(nativeIdle,plan,t=>HollowSourceSkin.Sample(plan,reference,t*reference.Duration),allowZeroAddedBones:new HashSet<int>{0}),"Source body/root visibility allowlist accepted");
                var zero=Matrix4x4.CreateScale(0)*Matrix4x4.CreateTranslation(.1f,.2f,.3f);Reject(()=>NativeSpline.Encode([zero,zero],1,2,.0333333f,"Master",["DS1_L_shield"]),"Default encoder accepted hidden scale");
                Reject(()=>NativeSpline.Encode([zero,zero],1,2,.0333333f,"Master",["DS1_Master"],allowZeroScaleTracks:new HashSet<int>{0}),"Native encoder accepted hidden body/root scale");
                byte[] hidden=NativeSpline.Encode([zero,zero],1,2,.0333333f,"Master",["DS1_L_shield"],allowZeroScaleTracks:new HashSet<int>{0});var hiddenTrack=NativeTracks(hidden)[0][0];Check(hiddenTrack.StaticScale==Vector3.Zero&&At(hiddenTrack,0)==zero,"Authored zero visibility scale did not survive native HKX serialization");
                reports.Add(new{model,masks=masksArray,bones=plan.Added.Count,total=plan.Count,visibility=visibility.Select(i=>plan.Added[i].Name),clips,poses,shieldMotion});
                Console.WriteLine($"PASS {model} [{string.Join(',',masksArray)}]: {plan.Added.Count} source bones, {clips} clips/{poses} complete poses");
            }
        }
        Check(sawAnimatedShield&&verticesChecked>100000&&emitted>100,"Full source equipment/frame coverage missing");foreach(var (path,hash) in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==hash,"Input changed: "+path);
        File.WriteAllText(Path.Combine(output,"regression.json"),JsonSerializer.Serialize(new{status="PASS",sourcePoses,nativePoses,verticesChecked,fullFrames,emitted,hiddenFrames,worstSourceError,worstNativeError,reports,hashes},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS {emitted} source-motion/native-HKX cases, {nativePoses} complete poses, {verticesChecked} vertices, {hashes.Count} unchanged inputs; native gameplay pending");
    }
}

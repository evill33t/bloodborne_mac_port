// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class HollowCombatCharactersTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static Matrix4x4 Inverse(Matrix4x4 m){Check(Matrix4x4.Invert(m,out var result),"Singular test frame");return result;}
    static Matrix4x4 Matrix(Vector3 p,Quaternion q,Vector3 s)=>Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(p);
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{a.M11-b.M11,a.M12-b.M12,a.M13-b.M13,a.M21-b.M21,a.M22-b.M22,a.M23-b.M23,a.M31-b.M31,a.M32-b.M32,a.M33-b.M33,a.M41-b.M41,a.M42-b.M42,a.M43-b.M43}.Max(Math.Abs);
    static Matrix4x4 At(Spline.TransformTrack t,float frame,SourceAnimation.Bone? fallback=null) {
        Vector3 Vector(bool scale) {
            var types=scale?t.Mask.ScaleTypes:t.Mask.PositionTypes;var spline=scale?t.SplineScale:t.SplinePosition;
            var result=scale?(fallback?.Scale??Vector3.One):(fallback?.Translation??Vector3.Zero);var stat=scale?t.StaticScale:t.StaticPosition;
            for(int axis=0;axis<3;axis++) {
                if(spline!=null)result[axis]=(axis==0?spline.GetValueX(frame):axis==1?spline.GetValueY(frame):spline.GetValueZ(frame))??result[axis];
                else if(types.Contains((Spline.FlagOffset)(1<<axis)))result[axis]=stat[axis];
            }
            return result;
        }
        return Matrix(Vector(false),t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.HasStaticRotation?t.StaticRotation:fallback?.Rotation??Quaternion.Identity,Vector(true));
    }
    sealed class IndependentSource {
        readonly List<Spline.TransformTrack[]> blocks;readonly int[] bindings;internal readonly SourceAnimation Info;
        internal IndependentSource(byte[] skeleton,byte[] bytes) {
            Info=SourceAnimation.Decode(skeleton,bytes);var tag=new SourceCollision.Tag(bytes);
            var anim=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkaSplineCompressedAnimation");var binding=tag.Items.Single(i=>i.Count==1&&tag.Name(i.Type)=="hkaAnimationBinding");
            var data=tag.Array(anim,"data");var map=tag.Array(binding,"transformTrackToBoneIndices");bindings=Enumerable.Range(0,map.Count).Select(i=>(int)unchecked((short)tag.UShort(tag.Element(map,i).At))).ToArray();
            blocks=Spline.ReadSplineCompressedAnimByteBlock(false,Enumerable.Range(data.At,data.Count).Select(tag.Byte).ToArray(),bindings.Length,tag.Int(tag.Nested(anim.Type,anim.At,"numBlocks").At));
        }
        internal Matrix4x4[] Sample(float fraction) {
            float frame=(Info.FrameCount-1)*fraction;int block=Math.Min((int)(frame/255),blocks.Count-1);
            var local=Info.Bones.Select(b=>Matrix(b.Translation,b.Rotation,b.Scale)).ToArray();
            for(int i=0;i<bindings.Length;i++)local[bindings[i]]=At(blocks[block][i],frame-block*255,Info.Bones[bindings[i]]);
            return NativeAnimation.Worlds(local,Info.Bones.Select(b=>b.Parent).ToArray());
        }
    }
    static List<Spline.TransformTrack[]> NativeTracks(byte[] bytes) {
        var p=new NativeAnimation.Pack(bytes);int a=p.Object("hkaSplineCompressedAnimation");var data=p.Array(a+152,1,64*1024*1024);
        return Spline.ReadSplineCompressedAnimByteBlock(false,bytes.AsSpan(p.Data+data.At,data.Count).ToArray(),p.Int(a+20),p.Int(a+60));
    }
    static void NativeUnchanged(byte[] before,byte[] after,int originalBones,int total) {
        var a=new NativeAnimation.Pack(before);var b=new NativeAnimation.Pack(after);int aa=a.Object("hkaSplineCompressedAnimation"),ba=b.Object("hkaSplineCompressedAnimation");
        var ad=a.Array(aa+152,1,64*1024*1024);var bd=b.Array(ba+152,1,64*1024*1024);
        var ast=a.Array(aa+88,4,256);var bst=b.Array(ba+88,4,256);var end=a.Array(aa+104,4,256);
        Check(ast.Count==bst.Count&&b.Int(ba+20)==total,"Native block/track coverage changed");
        for(int block=0;block<ast.Count;block++) {
            int old=a.Int(ast.At+block*4),now=b.Int(bst.At+block*4),used=a.Int(end.At+block*4);
            Check(before.AsSpan(a.Data+ad.At+old,originalBones*4).SequenceEqual(after.AsSpan(b.Data+bd.At+now,originalBones*4)),"Native compressed channel masks changed");
            Check(before.AsSpan(a.Data+ad.At+old+originalBones*4,used-originalBones*4).SequenceEqual(after.AsSpan(b.Data+bd.At+now+total*4,used-originalBones*4)),"Native controller payload changed");
        }
        var restored=after.AsSpan(b.Data,a.Size).ToArray();int binding=a.Object("hkaAnimationBinding");
        foreach(int field in new[]{aa+20,aa+68})before.AsSpan(a.Data+field,4).CopyTo(restored.AsSpan(field,4));
        foreach(int field in new[]{aa+88,aa+104,aa+152,binding+32})before.AsSpan(a.Data+field,16).CopyTo(restored.AsSpan(field,16));
        Check(restored.AsSpan().SequenceEqual(before.AsSpan(a.Data,a.Size)),"Native objects, duration, root motion or character data changed");
    }
    static string Event(TAE.Event e) {
        // TAE's reader includes alignment padding in an event's raw tail.
        // Event ownership, times and every nonpadding byte remain exact.
        var bytes=e.GetParameterBytes(false);int length=bytes.Length;while(length>0&&bytes[length-1]==0)length--;
        var g=e.Group;var d=g?.GroupData;return $"{e.Type}:{e.StartTime:R}:{e.EndTime:R}:{e.Unk04}:{Convert.ToHexString(bytes.AsSpan(0,length))}:{g?.GroupType}:{d?.DataType}:{d?.CutsceneEntityType}:{d?.CutsceneEntityIDPart1}:{d?.CutsceneEntityIDPart2}:{d?.Area}:{d?.Block}";
    }
    static int Jump(TAE.Event e)=>e.Type==0?BitConverter.ToInt32(e.GetParameterBytes(false)):-1;
    static bool Selected(FLVER2 m,FLVER2.Mesh mesh,IReadOnlySet<int> masks) {
        var match=System.Text.RegularExpressions.Regex.Match(m.Materials[mesh.MaterialIndex].Name,@"#(\d+)#");return !match.Success||masks.Contains(int.Parse(match.Groups[1].Value));
    }
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],definitions=args[2],output=args[3];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();
        byte[] Read(string root,string relative){string path=Path.Combine(root,relative);var b=File.ReadAllBytes(path);hashes.TryAdd(path,Convert.ToHexStringLower(SHA256.HashData(b)));return b;}
        byte[]? Source(string relative)=>File.Exists(Path.Combine(sourceRoot,relative))?Read(sourceRoot,relative):null;
        byte[] Native(string relative)=>Read(nativeRoot,relative);
        var sourceMap=MSB1.Read(Source("map/MapStudio/m10_01_00_00.msb")!);var sourceParams=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx")!);
        var rows=PARAM.Read(sourceParams.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);rows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,"NpcParam.xml")));
        var originalBank=BND4.Read(Native("chr/c2630.anibnd.dcx"));var originalCharacter=BND4.Read(Native("chr/c2630.chrbnd.dcx"));var originalTae=TAE.Read(originalBank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
        var originalRig=NativeAnimation.Skeleton(originalBank.Files.Single(f=>f.ID==1000000).Bytes.ToArray());var nativeHks=Native("action/script/c2630.hks");
        var reports=new List<object>();int checkedClips=0,checkedPoses=0,vertices=0,sockets=0;float worstFrame=0,worstVertex=0,worstSocket=0,worstInterpolationFrame=0;object? interpolationContext=null;
        foreach(var tuple in new[]{(Model:"c2550",Npc:255002,Think:255002,Role:HollowCombatProfiles.Kind.Crossbow,Target:"c9996"),(Model:"c2540",Npc:254010,Think:254000,Role:HollowCombatProfiles.Kind.SwordShield,Target:"c9997")}) {
            var actor=sourceMap.Parts.Enemies.First(a=>a.ModelName==tuple.Model&&a.NPCParamID==tuple.Npc&&a.ThinkParamID==tuple.Think&&a.InitAnimID==-1);
            var row=rows.Rows.Single(r=>r.ID==actor.NPCParamID);var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
            var profile=HollowCombatProfiles.Select(actor,masks)!;Check(profile.Role==tuple.Role,"Installed role identity changed");
            var authored=MapCharacters.Authored(Source,tuple.Model);var sourceModel=FLVER2.Read(authored.Model);var sourceBind=SkinRetarget.World(sourceModel.Nodes);var selected=sourceModel.Meshes.Where(m=>Selected(sourceModel,m,masks)).ToArray();
            var files=new Dictionary<string,byte[]>();Console.WriteLine("Packaging "+tuple.Model+" "+tuple.Role);
            var result=HollowCombatCharacters.Package(Source,Native,profile,tuple.Target,authored.Model,authored.Textures,masks,null,files);
            File.WriteAllBytes(Path.Combine(output,tuple.Target+".chrbnd.dcx"),files["chr/"+tuple.Target+".chrbnd.dcx"]);File.WriteAllBytes(Path.Combine(output,tuple.Target+".anibnd.dcx"),files["chr/"+tuple.Target+".anibnd.dcx"]);
            Check(files.Count==3&&files["action/script/"+tuple.Target+".hks"].SequenceEqual(nativeHks),"Native HKS or package closure changed");
            var bank=BND4.Read(files["chr/"+tuple.Target+".anibnd.dcx"]);var character=BND4.Read(files["chr/"+tuple.Target+".chrbnd.dcx"]);var visual=FLVER2.Read(character.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);var bind=SkinRetarget.World(visual.Nodes);
            var ownedDeaths=result.Report.OwnedDeathClips.ToDictionary(b=>b.Action);
            Check(result.Report.MotionClips==567&&ownedDeaths.Keys.Order().SequenceEqual(new[]{2002211,2002261,3002252,3002253})&&bank.Files.Count==originalBank.Files.Count+4&&bank.Files.Select(f=>f.ID).Order().SequenceEqual(originalBank.Files.Select(f=>f.ID).Concat(ownedDeaths.Keys.Select(i=>1000000000+i)).Order()),"Owned death binder membership changed");
            Check(result.Report.DeathPhysics is {Bodies:19,Constraints:18},"Source death physics report missing");
            var physics=new HollowDeathRig.Pack(character.Files.Single(f=>f.ID==300).Bytes.ToArray());
            var physicalRig=physics.Objects.Where(o=>o.Name=="hkaSkeleton").Select(o=>physics.Skeleton(o.At)).Single(s=>s.Names.Length>24);
            var serializedRig=NativeAnimation.Skeleton(bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
            Check(physicalRig.Names.SequenceEqual(serializedRig.Names)&&physicalRig.Parents.Select(p=>(int)p).SequenceEqual(serializedRig.Parents)&&physicalRig.Local.Zip(serializedRig.Local).All(p=>Error(p.First,p.Second)<.00001f),"Packaged ragdoll animation skeleton differs from live ANIBND");
            var expectedProject=SkinRetarget.RenameBehavior(originalBank.Files.Single(f=>f.ID==7000300).Bytes,"c2630",tuple.Target).ToArray();
            foreach(var death in ownedDeaths.Values)System.Text.Encoding.ASCII.GetBytes(tuple.Target).CopyTo(expectedProject,death.ProjectOffset+@"Model\chr\".Length);
            Check(expectedProject.SequenceEqual(bank.Files.Single(f=>f.ID==7000300).Bytes.ToArray()),"Changes escaped the four owned death project paths");
            foreach(var member in originalCharacter.Files.Where(f=>f.ID!=300&&!Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)&&!Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)&&!Leaf(f.Name).EndsWith(".clm2",StringComparison.OrdinalIgnoreCase)&&!Leaf(f.Name).Equals("div_data.txt",StringComparison.OrdinalIgnoreCase)&&!TPF.Is(f.Bytes.ToArray()))) {
                var copied=character.Files.Single(f=>f.ID==member.ID);Check(member.Bytes.Span.SequenceEqual(copied.Bytes.Span)&&copied.Name==member.Name.Replace("c2630",tuple.Target,StringComparison.OrdinalIgnoreCase),"Unrelated native character member changed");
            }
            var rig=NativeAnimation.Skeleton(bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray());Check(rig.Names.Take(84).SequenceEqual(originalRig.Names)&&rig.Local.Take(84).SequenceEqual(originalRig.Local)&&rig.Parents.Take(84).SequenceEqual(originalRig.Parents),"Native84-bone controller prefix changed");
            int active=visual.Nodes.Count(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0);Check(visual.Nodes.Take(active).All(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0)&&visual.Nodes.Skip(active).All(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)!=0),"Render active-prefix contract lost");
            Check(selected.Length==visual.Meshes.Count&&result.Skin.DirectWeightShare>.999999,"Selected source body/equipment lost");
            for(int m=0;m<selected.Length;m++) {
                Check(selected[m].Vertices.Count==visual.Meshes[m].Vertices.Count,"Mesh vertex population changed");
                var sourceFaces=selected[m].FaceSets.Where(f=>(f.Flags&FLVER2.FaceSet.FSFlags.EdgeCompressed)==0).ToArray();Check(sourceFaces.Length==visual.Meshes[m].FaceSets.Count,"Source LOD face-set population changed");
                for(int set=0;set<sourceFaces.Length;set++)Check(sourceFaces[set].Flags==visual.Meshes[m].FaceSets[set].Flags&&sourceFaces[set].CullBackfaces==visual.Meshes[m].FaceSets[set].CullBackfaces&&sourceFaces[set].Triangulate(selected[m].Vertices.Count<65535).SequenceEqual(visual.Meshes[m].FaceSets[set].Triangulate(visual.Meshes[m].Vertices.Count<65535)),"Source equipment/LOD topology changed");
                foreach(var (v,i) in selected[m].Vertices.Select((v,i)=>(v,i)))Check(Vector3.Distance(v.Position,visual.Meshes[m].Vertices[i].Position)<.00001f,"Bind geometry changed");
                foreach(var v in visual.Meshes[m].Vertices)for(int slot=0;slot<4;slot++)if(v.BoneWeights[slot]>0)Check(SkinRetarget.Bone(visual.Meshes[m],v.BoneIndices[slot])<active,"Weighted source node outside render palette");
            }
            var owned=TPF.Read(character.Files.Single(f=>f.ID==100).Bytes);Check(owned.Textures.Count>0&&owned.Textures.All(t=>t.Name.StartsWith(tuple.Target+"_",StringComparison.Ordinal)),"Texture owner collision");
            var motion=HollowCombatMotion.Create(Source,tuple.Model,tuple.Role,originalBank);var sourceBank=BND3.Read(Source("chr/"+tuple.Model+".anibnd.dcx")!);var skeleton=sourceBank.Files.Single(f=>Leaf(f.Name).Equals("skeleton.hkx",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
            var sourceActions=TAE.Read(sourceBank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes).Animations.ToDictionary(a=>checked((int)a.ID));var decoded=new Dictionary<int,IndependentSource>();
            IndependentSource From(int id) {
                if(decoded.TryGetValue(id,out var old))return old;int physical=id;var seen=new HashSet<int>();TAE.Animation action;
                while(true){Check(seen.Add(physical),"Source motion cycle");action=sourceActions[physical];if(action.MiniHeader is TAE.Animation.AnimMiniHeader.ImportOtherAnim other){physical=other.ImportFromAnimID;continue;}if(action.MiniHeader is TAE.Animation.AnimMiniHeader.Standard standard&&standard.ImportsHKX&&standard.ImportHKXSourceAnimID>=0){physical=standard.ImportHKXSourceAnimID;continue;}break;}
                string name=Leaf(action.AnimFileName).Replace(".hkxwin",".hkx",StringComparison.OrdinalIgnoreCase);var bytes=sourceBank.Files.Single(f=>Leaf(f.Name).Equals(name,StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();var value=new IndependentSource(skeleton,bytes);decoded.Add(id,value);return value;
            }
            var reference=From(0);var names=reference.Info.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(x=>x.Name,x=>x.i);var renderToRig=visual.Nodes.Select(n=>Array.IndexOf(rig.Names,n.Name)).ToArray();
            FLVER.Dummy? sourceSocket=profile.Ranged?sourceModel.Dummies.Single(d=>d.ReferenceID==profile.SourceProjectileDummy):null;FLVER.Dummy? socket=profile.Ranged?visual.Dummies.Single(d=>d.ReferenceID==60):null;
            if(socket!=null)Check(visual.Nodes[socket.AttachBoneIndex].Name=="DS1_"+sourceModel.Nodes[sourceSocket!.AttachBoneIndex].Name&&socket.FollowAttachBone==sourceSocket.FollowAttachBone&&socket.UseUpwardVector==sourceSocket.UseUpwardVector,"Muzzle lost animated source attachment after render sorting");
            int localPoses=0;
            foreach(var file in bank.Files.Where(f=>f.ID>=1000000000)) {
                int id=file.ID-1000000000;int originalID=ownedDeaths.TryGetValue(id,out var death)?death.NativeCounterpart:id;int sourceID=death?.SourceAction??motion.MotionSourceIDs[id];byte[] before=originalBank.Files.Single(f=>f.ID==1000000000+originalID).Bytes.ToArray(),after=file.Bytes.ToArray();NativeUnchanged(before,after,84,rig.Names.Length);checkedClips++;
                var p=new NativeAnimation.Pack(after);int a=p.Object("hkaSplineCompressedAnimation"),binding=p.Object("hkaAnimationBinding");var tracks=NativeTracks(after);bool additive=after[p.Data+binding+80]==1;
                var source=motion.ReferenceFallbackIDs.Contains(id)?reference:From(sourceID);
                int lastFrame=p.Int(a+56)-1;
                foreach(float fraction in new[]{0f,(lastFrame/2)/(float)lastFrame,1f,.5f}.Distinct()) {
                    float frame=(p.Int(a+56)-1)*fraction;int block=Math.Min((int)(frame/255),tracks.Count-1);var local=tracks[block].Select(t=>At(t,frame-block*255)).ToArray();
                    if(additive){Check(local.Skip(84).All(m=>Error(m,Matrix4x4.Identity)<.000001f),"Added source additive tracks must be identity");continue;}
                    var world=NativeAnimation.Worlds(local,rig.Parents);var sourceWorld=source.Sample(motion.ReferenceFallbackIDs.Contains(id)?0:fraction);
                    // Native channels are sampled at native keyframes. Between
                    // keys they linearly approximate source splines; report
                    // that distinct interpolation limit instead of labeling it
                    // a source hierarchy or serialized-keyframe mismatch.
                    if(Math.Abs(frame-MathF.Round(frame))>.0001f) {
                        for(int bone=84;bone<rig.Names.Length;bone++) {
                            var expected=sourceWorld[names[rig.Names[bone][4..]]];float error=Error(expected,world[bone]);
                            if(error>worstInterpolationFrame){worstInterpolationFrame=error;interpolationContext=new{model=tuple.Model,physical=id,source=sourceID,bone=rig.Names[bone],fraction,nativeFrames=p.Int(a+56),sourceFrames=source.Info.FrameCount,translationError=Vector3.Distance(expected.Translation,world[bone].Translation)};}
                        }
                        continue;
                    }
                    for(int bone=84;bone<rig.Names.Length;bone++){
                        float error=Error(sourceWorld[names[rig.Names[bone][4..]]],world[bone]);worstFrame=Math.Max(worstFrame,error);
                        Check(error<.006f,"Emitted source bone frame mismatch: "+tuple.Model+"/"+id+"/"+rig.Names[bone]+" error "+error);
                    }
                    for(int m=0;m<selected.Length;m++)for(int vi=0;vi<selected[m].Vertices.Count;vi++) {
                        var sv=selected[m].Vertices[vi];var tv=visual.Meshes[m].Vertices[vi];Vector3 expected=Vector3.Zero,actual=Vector3.Zero;
                        for(int slot=0;slot<4;slot++) {
                            if(sv.BoneWeights[slot]>0){int bone=SkinRetarget.Bone(selected[m],sv.BoneIndices[slot]);expected+=Vector3.Transform(sv.Position,Inverse(sourceBind[bone])*sourceWorld[names[sourceModel.Nodes[bone].Name]])*sv.BoneWeights[slot];}
                            if(tv.BoneWeights[slot]>0){int bone=SkinRetarget.Bone(visual.Meshes[m],tv.BoneIndices[slot]);actual+=Vector3.Transform(tv.Position,Inverse(bind[bone])*world[renderToRig[bone]])*tv.BoneWeights[slot];}
                        }
                        float error=Vector3.Distance(expected,actual);worstVertex=Math.Max(worstVertex,error);Check(error<.01f,"Full source body/equipment vertex mismatch: "+tuple.Model+"/"+id+" error "+error);vertices++;
                    }
                    if(socket!=null) {
                        var ss=sourceSocket!;var old=ss.ParentBoneIndex<0?Matrix4x4.Identity:sourceBind[ss.ParentBoneIndex];var now=bind[socket.ParentBoneIndex];
                        if(ss.FollowAttachBone&&ss.AttachBoneIndex>=0){old*=Inverse(sourceBind[ss.AttachBoneIndex])*sourceWorld[names[sourceModel.Nodes[ss.AttachBoneIndex].Name]];now*=Inverse(bind[socket.AttachBoneIndex])*world[renderToRig[socket.AttachBoneIndex]];}
                        foreach(var pair in new[]{(Vector3.Transform(ss.Position,old),Vector3.Transform(socket.Position,now)),(Vector3.TransformNormal(ss.Forward,old),Vector3.TransformNormal(socket.Forward,now)),(Vector3.TransformNormal(ss.Upward,old),Vector3.TransformNormal(socket.Upward,now))}) {
                            float error=Vector3.Distance(pair.Item1,pair.Item2);worstSocket=Math.Max(worstSocket,error);Check(error<.003f,"Source muzzle frame changed after active-prefix sorting: "+id);
                        }
                        sockets++;
                    }
                    checkedPoses++;localPoses++;
                }
            }
            var tae=TAE.Read(bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);Check(tae.Animations.Count==688&&tae.ID==200000+int.Parse(tuple.Target[1..]),"Serialized action identity/population changed");
            var mappings=motion.Mappings.ToDictionary(m=>m.NativeID);int hits=0,shots=0;
            foreach(var after in tae.Animations) {
                Check(after.Events.All(e=>e.Group==null||after.EventGroups.Contains(e.Group)),"Serialized event references another action's group");
                var before=originalTae.Animations.Single(a=>a.ID==after.ID);Check(before.AnimFileName==after.AnimFileName&&before.MiniHeader.Type==after.MiniHeader.Type,"Native playback alias changed");
                if(!mappings.TryGetValue(checked((int)after.ID),out var map)||!map.Combat)Check(before.Events.Select(Event).SequenceEqual(after.Events.Select(Event)),"Unmapped/noncombat native action changed");
                else {
                    var retained=before.Events.Where(e=>e.Type is not(1 or 2)&&Jump(e) is not(23 or 78 or 79 or 86)).Select(Event).ToArray();Check(retained.SequenceEqual(after.Events.Take(retained.Length).Select(Event)),"Native effect/sound/physics event changed");
                    var expected=motion.Action(map.NativeID);var timeline=new NativeAnimation.Pack(bank.Files.Single(f=>f.ID==1000000000+motion.NativeMotionID(map.NativeID)).Bytes.ToArray());float duration=timeline.Float(timeline.Object("hkaSplineCompressedAnimation")+16);
                    foreach(int type in new[]{1,2}) {
                        var sourceEvents=expected.Events.Where(e=>e.Type==type).ToArray();var actual=after.Events.Where(e=>e.Type==type).ToArray();Check(actual.Length==sourceEvents.Length,"Source hit/shot multiplicity lost in final BND");
                        for(int i=0;i<actual.Length;i++)Check(Math.Abs(actual[i].StartTime-sourceEvents[i].StartTime*duration/expected.Clip.Duration)<.0001f&&Math.Abs(actual[i].EndTime-sourceEvents[i].EndTime*duration/expected.Clip.Duration)<.0001f,"Serialized source hit/shot window disagrees with visible pose");
                        if(type==1)hits+=actual.Length;else shots+=actual.Length;
                    }
                }
            }
            foreach(var original in originalBank.Files.Where(f=>f.ID<1000000000&&f.ID!=1000000&&!TAE.Is(f.Bytes.ToArray())&&f.ID is not(>=7000000 and <8000000)))Check(original.Bytes.Span.SequenceEqual(bank.Files.Single(f=>f.ID==original.ID).Bytes.Span),"Unrelated native animation member changed");
            Check(character.Files.Count(f=>TPF.Is(f.Bytes.ToArray()))==1&&character.Files.All(f=>!Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)),"Unsupported cloth or duplicate texture resource in final package");
            reports.Add(new{tuple.Model,role=tuple.Role.ToString(),nativeBones=84,addedBones=rig.Names.Length-84,clips=571,actions=688,poses=localPoses,hits,shots,vertices=result.Skin.Vertices,fallbacks=motion.ReferenceFallbackIDs.Count});
            Console.WriteLine($"PASS {tuple.Model} {tuple.Role}: 571 clips/688 actions/{localPoses} complete poses, {hits} hit/{shots} shot windows");
        }
        foreach(var(path,hash)in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==hash,"Input changed: "+path);
        File.WriteAllText(Path.Combine(output,"regression.json"),JsonSerializer.Serialize(new{status="PASS",checkedClips,checkedPoses,vertices,sockets,worstFrame,worstVertex,worstSocket,worstInterpolationFrame,interpolationContext,reports,hashes},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS {checkedClips} serialized clips, {checkedPoses} complete poses, {vertices} vertices, {sockets} socket frames, {hashes.Count} unchanged inputs; native gameplay pending");
    }
}

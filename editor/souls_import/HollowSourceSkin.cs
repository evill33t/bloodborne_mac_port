// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text.RegularExpressions;

// The visible source hierarchy is independent of the native combat controller.
// Keep equipment's animated source frames instead of reducing them to an idle
// socket. Native attack behavior/timing and physical hitboxes remain separate.
internal static class HollowSourceSkin {
    const string Prefix="DS1_";
    static string Key(string name)=>new(name.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
    static bool Equipment(string name)=>Regex.IsMatch(Key(name),@"\A(?:rweapon\d*|lweapon\d*|lshield|sword|bpw00)\z");
    static void Projectile(FLVER2 model,FLVER.Dummy dummy) {
        if(dummy.ReferenceID is 20 or 30)return;
        // The authored c2540 firebomb event uses dummy1. Its disconnected
        // Model_Dmy frame supplies the rest socket and R Hand animates it.
        // Keep this addition bounded; other characters' dummy1 is unrelated.
        if(dummy.ReferenceID!=1||!model.Nodes.Any(n=>n.Name=="c2540_weapon")||
            dummy.ParentBoneIndex<0||dummy.ParentBoneIndex>=model.Nodes.Count||model.Nodes[dummy.ParentBoneIndex].Name!="Model_Dmy"||
            dummy.AttachBoneIndex<0||dummy.AttachBoneIndex>=model.Nodes.Count||model.Nodes[dummy.AttachBoneIndex].Name!="R Hand"||
            !dummy.FollowAttachBone||!dummy.UseUpwardVector||dummy.Unk30!=0||dummy.Unk34!=0||
            Vector3.Distance(dummy.Position,new(-.76459473f,1.4338198f,-.009280578f))>.000001f||
            Vector3.Distance(dummy.Forward,new(0,0,-.27979785f))>.000001f||
            Vector3.Distance(dummy.Upward,new(0,.08235574f,0))>.000001f)
            throw new InvalidDataException("Unsupported source hollow projectile dummy");
    }
    static int AnimatedOwner(FLVER2 source,int node,IReadOnlySet<string> animated) {
        if(node<0||node>=source.Nodes.Count)throw new InvalidDataException("Invalid source projectile owner");int at=node,steps=0;
        while(at>=0) {
            if(at>=source.Nodes.Count||++steps>source.Nodes.Count)throw new InvalidDataException("Invalid source projectile owner hierarchy");
            if(animated.Contains(source.Nodes[at].Name))return at;
            // The shipped Model_Dmy is a static, disconnected authoring frame;
            // it has no animation channel. Its root space is source Master.
            if(at==node&&Key(source.Nodes[at].Name)!="modeldmy")throw new InvalidDataException("Unrecognized unanimated source projectile owner: "+source.Nodes[at].Name);
            at=source.Nodes[at].ParentIndex;
        }
        int master=source.Nodes.FindIndex(n=>n.Name=="Master");
        if(master<0||!animated.Contains("Master"))throw new InvalidDataException("Source projectile root frame missing");return master;
    }
    static bool Selected(FLVER2 model,FLVER2.Mesh mesh,IReadOnlySet<int>? masks) {
        var match=Regex.Match(model.Materials[mesh.MaterialIndex].Name,@"#(\d+)#");return masks==null||!match.Success||masks.Contains(int.Parse(match.Groups[1].Value));
    }
    internal static byte[] PrepareModel(byte[] sourceModel,SourceClothPose.Result? settled=null) {
        if(settled==null)return sourceModel.ToArray();
        var original=FLVER2.Read(sourceModel);var model=FLVER2.Read(settled.Model);
        if(original.Nodes.Count!=model.Nodes.Count||original.Meshes.Count!=model.Meshes.Count||
            original.Nodes.Where((n,i)=>n.Name!=model.Nodes[i].Name||n.ParentIndex!=model.Nodes[i].ParentIndex||n.ComputeLocalTransform()!=model.Nodes[i].ComputeLocalTransform()).Any()||
            settled.Report.RigidCapeAnchor!="Spine1"||!settled.Report.CapeFixedAnchors.Order().SequenceEqual(new[]{"L UpperArm","R UpperArm","Spine1"})||
            !settled.Report.CapeMeshes.Order().SequenceEqual(new[]{3,4})||settled.Report.SourcePhysicsSHA256.Length!=64)
            throw new InvalidDataException("Static source cape provenance/hierarchy changed");
        ValidateAnchors(model,settled.Anchors,false);
        foreach(var (index,name) in settled.Anchors) {
            int bone=model.Nodes.FindIndex(n=>n.Name==name);var mesh=model.Meshes[index];mesh.BoneIndices.Clear();
            foreach(var vertex in mesh.Vertices){vertex.BoneIndices=new();vertex.BoneWeights=new();vertex.BoneIndices[0]=bone;vertex.BoneWeights[0]=1;}
        }
        var bytes=model.Write();ValidateAnchors(FLVER2.Read(bytes),settled.Anchors,true);return bytes;
    }
    static void ValidateAnchors(FLVER2 model,IReadOnlyDictionary<int,string> anchors,bool prepared) {
        if(anchors.Count!=2||!anchors.Keys.Order().SequenceEqual(new[]{3,4})||anchors.Values.Any(n=>n!="Spine1")||
            model.Nodes.Count(n=>n.Name=="Spine1")!=1||!model.Nodes.Any(n=>n.Name=="c2560_weapon")||
            anchors.Keys.Any(i=>i>=model.Meshes.Count||model.Materials[model.Meshes[i].MaterialIndex].Name!="#07#")||
            anchors.Keys.Sum(i=>model.Meshes[i].Vertices.Count)!=337)
            throw new InvalidDataException("Unsupported static source cape mesh/anchor");
        if(prepared)foreach(int index in anchors.Keys)foreach(var v in model.Meshes[index].Vertices)
            if(v.BoneWeights[0]!=1||Enumerable.Range(1,3).Any(i=>v.BoneWeights[i]!=0)||model.Nodes[SkinRetarget.Bone(model.Meshes[index],v.BoneIndices[0])].Name!="Spine1")
                throw new InvalidDataException("Prepare the settled source cape weights before appending its visible rig");
    }
    internal static NativeRigAppend.Plan Rig(byte[] sourceModel,byte[] nativeModel,byte[] nativeSkeleton,SourceAnimation reference,
        IReadOnlyDictionary<int,string>? rigidAnchors=null,IReadOnlySet<int>? masks=null,IReadOnlyCollection<int>? sourceMuzzleIDs=null) {
        var source=FLVER2.Read(sourceModel);var native=NativeAnimation.Skeleton(nativeSkeleton);
        if(native.Names.Length is not(80 or 84)||native.Name!="Master"||native.Names[0]!="Master"||native.Parents[0]!=-1||
            source.Meshes.Count==0||source.Meshes.Count>1000||source.Meshes.Sum(m=>(long)m.Vertices.Count)>1000000||
            source.Meshes.Any(m=>m.Dynamic!=1||m.MaterialIndex<0||m.MaterialIndex>=source.Materials.Count)||
            reference.Bones.Count is <1 or >176||reference.Bones[0].Name!="Master"||reference.Bones[0].Parent!=-1||reference.Bones.Count(b=>b.Parent<0)!=1)
            throw new InvalidDataException("Source hollow/native controller rig contract changed");
        if(rigidAnchors is {Count:>0})ValidateAnchors(source,rigidAnchors,true);
        var sourceNames=source.Nodes.Select((n,i)=>(n.Name,i)).ToDictionary(v=>v.Name,v=>v.i,StringComparer.Ordinal);
        var bones=reference.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(v=>v.Name,v=>v.i,StringComparer.Ordinal);
        if(!sourceNames.TryGetValue("Master",out int master)||!sourceNames.TryGetValue("Pelvis",out int pelvis)||
            source.Nodes[pelvis].ParentIndex!=-1||!bones.TryGetValue("Pelvis",out int pelvisBone)||reference.Bones[pelvisBone].Parent!=0||
            !Matrix4x4.Invert(native.Local[0],out var inverseRoot))
            throw new InvalidDataException("Source hollow disconnected Pelvis/Master bind contract changed");
        var needed=new HashSet<int>();var order=new List<int>();var visiting=new HashSet<int>();
        void Add(int bone) {
            if(needed.Contains(bone))return;
            if(bone<0||bone>=reference.Bones.Count||!visiting.Add(bone))throw new InvalidDataException("Invalid source visible bind/animation hierarchy");
            var b=reference.Bones[bone];if(!sourceNames.TryGetValue(b.Name,out int node))throw new InvalidDataException("Source visible animation bone has no FLVER bind: "+b.Name);
            if(b.Parent>=0)Add(b.Parent);
            int parent=source.Nodes[node].ParentIndex;
            if(parent>=0) {if(parent>=source.Nodes.Count||!bones.TryGetValue(source.Nodes[parent].Name,out int index))throw new InvalidDataException("Source bind parent has no animation reference");Add(index);}
            visiting.Remove(bone);needed.Add(bone);order.Add(bone);
        }
        int vertices=0;
        foreach(var mesh in source.Meshes.Where(m=>Selected(source,m,masks)))foreach(var v in mesh.Vertices) {
            vertices++;float sum=0;
            for(int slot=0;slot<4;slot++) {
                float weight=v.BoneWeights[slot];if(!float.IsFinite(weight)||weight<0)throw new InvalidDataException("Invalid source hollow skin weight");sum+=weight;if(weight==0)continue;
                int node=SkinRetarget.Bone(mesh,v.BoneIndices[slot]);
                if(node<0||node>=source.Nodes.Count||!bones.TryGetValue(source.Nodes[node].Name,out int bone))throw new InvalidDataException("Weighted source hollow bone has no animation reference");Add(bone);
            }
            if(Math.Abs(sum-1)>.001f)throw new InvalidDataException("Unnormalized source hollow skin");
        }
        if(sourceMuzzleIDs!=null)foreach(int id in sourceMuzzleIDs.Distinct()) {
            var matches=source.Dummies.Where(d=>d.ReferenceID==id).ToArray();
            if(matches.Length!=1)throw new InvalidDataException("Missing/ambiguous source projectile dummy");
            Projectile(source,matches[0]);
            foreach(int node in new[]{(int)matches[0].ParentBoneIndex,(int)matches[0].AttachBoneIndex}) {
                if(node<0)continue;
                int owner=AnimatedOwner(source,node,bones.Keys.ToHashSet(StringComparer.Ordinal));Add(bones[source.Nodes[owner].Name]);
            }
        }
        if(vertices==0||order.Count==0||order[0]!=0||native.Names.Length+order.Count>256)throw new InvalidDataException("Source visible closure exceeds the native palette");
        var indices=order.Select((b,i)=>(b,i)).ToDictionary(v=>v.b,v=>v.i);int count=native.Names.Length;
        var added=order.Select(bone=> {
            var b=reference.Bones[bone];int node=sourceNames[b.Name];var bind=new FLVER.Node(source.Nodes[node]){Name=Prefix+b.Name};
            bind.ParentIndex=bind.ParentIndex<0?(short)-1:checked((short)(count+indices[bones[source.Nodes[bind.ParentIndex].Name]]));
            var local=Matrix4x4.CreateScale(b.Scale)*Matrix4x4.CreateFromQuaternion(b.Rotation)*Matrix4x4.CreateTranslation(b.Translation);
            return new NativeRigAppend.Bone(bind.Name,b.Parent<0?0:count+indices[b.Parent],b.Parent<0?local*inverseRoot:local,bind);
        }).ToArray();
        return NativeRigAppend.Create(nativeModel,nativeSkeleton,added);
    }
    internal static IReadOnlySet<int> HiddenEquipmentBones(NativeRigAppend.Plan plan) {
        var result=new HashSet<int>();int first=plan.Original.Names.Length;
        for(int i=0;i<plan.Added.Count;i++) {
            int at=i,steps=0;
            while(at>=0) {
                if(++steps>plan.Added.Count||!plan.Added[at].Name.StartsWith(Prefix,StringComparison.Ordinal))throw new InvalidDataException("Invalid source visibility hierarchy");
                if(Equipment(plan.Added[at].Name[Prefix.Length..])){result.Add(i);break;}
                int parent=plan.Added[at].Parent;at=parent<first?-1:parent-first;
            }
        }
        return result;
    }
    internal static IReadOnlyDictionary<string,string> Overrides(NativeRigAppend.Plan plan)=>plan.Added.ToDictionary(b=>b.Name[Prefix.Length..],b=>b.Name,StringComparer.Ordinal);
    internal static FLVER.Dummy RebindDummy(FLVER.Dummy dummy,byte[] sourceModel,NativeRigAppend.Plan plan,short targetReference) {
        if(targetReference<0)throw new InvalidDataException("Unsupported target projectile dummy reference");
        var source=FLVER2.Read(sourceModel);var target=FLVER2.Read(plan.Model);var sw=SkinRetarget.World(source.Nodes);var tw=SkinRetarget.World(target.Nodes);
        Projectile(source,dummy);
        var names=Overrides(plan).Keys.ToHashSet(StringComparer.Ordinal);int parent=dummy.ParentBoneIndex<0?source.Nodes.FindIndex(n=>n.Name=="Master"):AnimatedOwner(source,dummy.ParentBoneIndex,names);
        if(parent<0)throw new InvalidDataException("Source projectile parent missing");int index=target.Nodes.FindIndex(n=>n.Name==Prefix+source.Nodes[parent].Name);
        if(index<0||!Matrix4x4.Invert(sw[parent],out var inverse))throw new InvalidDataException("Source projectile target parent missing/singular");
        var before=dummy.ParentBoneIndex<0?Matrix4x4.Identity:sw[dummy.ParentBoneIndex];var delta=before*inverse;
        var result=new FLVER.Dummy(dummy){ReferenceID=targetReference,ParentBoneIndex=checked((short)index),Position=Vector3.Transform(dummy.Position,delta),Forward=Vector3.TransformNormal(dummy.Forward,delta),Upward=Vector3.TransformNormal(dummy.Upward,delta)};
        if(dummy.AttachBoneIndex>=0) {
            int attach=AnimatedOwner(source,dummy.AttachBoneIndex,names),mapped=target.Nodes.FindIndex(n=>n.Name==Prefix+source.Nodes[attach].Name);
            if(mapped<0)throw new InvalidDataException("Source projectile target attachment missing");result.AttachBoneIndex=checked((short)mapped);
        }
        if(!float.IsFinite(result.Position.LengthSquared())||!float.IsFinite(result.Forward.LengthSquared())||!float.IsFinite(result.Upward.LengthSquared())||
            Vector3.Distance(Vector3.Transform(dummy.Position,before),Vector3.Transform(result.Position,tw[index]))>.00001f||
            Vector3.Distance(Vector3.TransformNormal(dummy.Forward,before),Vector3.TransformNormal(result.Forward,tw[index]))>.00001f||
            Vector3.Distance(Vector3.TransformNormal(dummy.Upward,before),Vector3.TransformNormal(result.Upward,tw[index]))>.00001f)
            throw new InvalidDataException("Source projectile rest frame changed during binding");
        return result;
    }
    internal static Matrix4x4[] Sample(NativeRigAppend.Plan plan,SourceAnimation clip,float time) {
        var names=clip.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(v=>v.Name,v=>v.i,StringComparer.Ordinal);var hidden=HiddenEquipmentBones(plan);var values=new Matrix4x4[plan.Added.Count];
        for(int i=0;i<values.Length;i++) {
            string name=plan.Added[i].Name[Prefix.Length..];if(!names.TryGetValue(name,out int bone))throw new InvalidDataException("Source clip is missing a visible bone: "+name);
            int parent=plan.Added[i].Parent-plan.Original.Names.Length,sourceParent=clip.Bones[bone].Parent;
            if(parent<0?sourceParent!=-1:sourceParent<0||clip.Bones[sourceParent].Name!=plan.Added[parent].Name[Prefix.Length..])throw new InvalidDataException("Source visible clip hierarchy changed");
            values[i]=clip.HollowLocal(bone,time,hidden.Contains(i));
        }
        if(!Matrix4x4.Invert(plan.Original.Local[0],out var inverse))throw new InvalidDataException("Singular native hollow root");values[0]*=inverse;return values;
    }
}

internal sealed partial class SourceAnimation {
    // Zero scale is an authored visibility channel only for a selected equipment
    // branch. It must survive to the explicitly allowlisted native spline; do
    // not replace it with identity or a guessed tiny visible scale.
    internal Matrix4x4 HollowLocal(int bone,float time,bool hiddenEquipment) {
        if(bone<0||bone>=Bones.Count||!float.IsFinite(time)||time<0||time>Duration+.0001f)throw new InvalidDataException("Invalid source hollow sample");
        float frame=time>=Duration?FrameCount-1:Math.Min(time/FrameDuration,FrameCount-1);int block=Math.Min((int)(frame/(framesPerBlock-1)),blocks.Length-1);float local=frame-block*(framesPerBlock-1);
        var b=Bones[bone];int track=Array.IndexOf(trackBones,bone);if(track<0)return Matrix(b.Translation,b.Rotation,b.Scale);
        var value=blocks[block][track];var scale=value.Scale.Value(local,b.Scale);
        if(hiddenEquipment) {if(!float.IsFinite(scale.LengthSquared())||Math.Abs(scale.X)>10000||Math.Abs(scale.Y)>10000||Math.Abs(scale.Z)>10000)throw new InvalidDataException("Invalid source equipment visibility scale");}
        else ValidScale(scale);
        return Matrix(value.Position.Value(local,b.Translation),value.Rotation?.Value(local)??b.Rotation,scale);
    }
}

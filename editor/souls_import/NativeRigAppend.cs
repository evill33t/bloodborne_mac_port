// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text;

// Extend a native playback rig without renumbering its bones or decoding its
// compressed body channels. Added skin bones and their motion have their own
// tracks; native behavior, locomotion, physics anchors and event timing remain.
internal static class NativeRigAppend {
    // Both indices address the FINAL compact FLVER/animation-rig order. Bind's
    // parent may differ from Parent (retail FLVERs have disconnected bind roots).
    internal sealed record Bone(string Name,int Parent,Matrix4x4 ReferenceLocal,FLVER.Node Bind);
    internal sealed record Plan(byte[] Model,byte[] Skeleton,NativeAnimation.Rig Original,IReadOnlyList<Bone> Added) {
        internal int Count=>Original.Names.Length+Added.Count;
    }
    internal sealed class Edit {
        readonly byte[] input;readonly int header,data,oldEnd,oldGlobal,oldVirtual;
        readonly List<byte> body;readonly SortedDictionary<int,int> pointers=new();
        internal readonly NativeAnimation.Pack Pack;
        internal Edit(byte[] bytes) {
            input=bytes;Pack=new(bytes);data=Pack.Data;header=64+BitConverter.ToUInt16(bytes,62)+128;
            int local=BitConverter.ToInt32(bytes,header+24);oldGlobal=BitConverter.ToInt32(bytes,header+28);oldVirtual=BitConverter.ToInt32(bytes,header+32);oldEnd=BitConverter.ToInt32(bytes,header+36);
            if(BitConverter.ToInt32(bytes,header+40)!=oldEnd||BitConverter.ToInt32(bytes,header+44)!=oldEnd)throw new InvalidDataException("Native rig has unsupported imports/exports");
            body=bytes.AsSpan(data,local).ToArray().ToList();
            for(int at=local;at+8<=oldGlobal;at+=8)if(BitConverter.ToInt32(bytes,data+at)!=-1)pointers.Add(BitConverter.ToInt32(bytes,data+at),BitConverter.ToInt32(bytes,data+at+4));
        }
        internal int Append(ReadOnlySpan<byte> bytes,int alignment=16) {while(body.Count%alignment!=0)body.Add(0);int at=body.Count;body.AddRange(bytes.ToArray());return at;}
        internal void Int(int at,int value){if(at<0||at>body.Count-4)throw new InvalidDataException("Invalid rig edit offset");var b=BitConverter.GetBytes(value);for(int i=0;i<4;i++)body[at+i]=b[i];}
        internal void Pointer(int field,int target){if(field<0||field>body.Count-8||target<0||target>=body.Count)throw new InvalidDataException("Invalid appended rig pointer");pointers[field]=target;}
        internal int Array(int field,int count,ReadOnlySpan<byte> bytes) {if(count<=0)throw new InvalidDataException("Cannot append an empty rig array");int at=Append(bytes);Pointer(field,at);Int(field+8,count);Int(field+12,count|int.MinValue);return at;}
        internal byte[] Finish() {
            while(body.Count%16!=0)body.Add(0);int local=body.Count;
            foreach(var pair in pointers){body.AddRange(BitConverter.GetBytes(pair.Key));body.AddRange(BitConverter.GetBytes(pair.Value));}
            while(body.Count%16!=0)body.Add(255);int global=body.Count;
            body.AddRange(input.AsSpan(data+oldGlobal,oldVirtual-oldGlobal).ToArray());int virtuals=body.Count;
            body.AddRange(input.AsSpan(data+oldVirtual,oldEnd-oldVirtual).ToArray());int end=body.Count;
            var result=input.AsSpan(0,data).ToArray().Concat(body).Concat(input.AsSpan(data+oldEnd).ToArray()).ToArray();
            int[] offsets=[local,global,virtuals,end,end,end];for(int i=0;i<offsets.Length;i++)BitConverter.GetBytes(offsets[i]).CopyTo(result,header+24+i*4);
            _=new NativeAnimation.Pack(result);return result;
        }
    }
    static byte[] Transform(Matrix4x4 m) {
        if(!Matrix4x4.Decompose(m,out var s,out var q,out var t)||Math.Abs(s.X)<.000001f||Math.Abs(s.Y)<.000001f||Math.Abs(s.Z)<.000001f||!float.IsFinite(q.LengthSquared()))throw new InvalidDataException("Invalid appended bone reference pose: "+m);
        q=Quaternion.Normalize(q);float[] values=[t.X,t.Y,t.Z,0,q.X,q.Y,q.Z,q.W,s.X,s.Y,s.Z,0];return values.SelectMany(BitConverter.GetBytes).ToArray();
    }
    internal static Plan Create(byte[] nativeModel,byte[] skeleton,IReadOnlyList<Bone> added) {
        var rig=NativeAnimation.Skeleton(skeleton);int original=rig.Names.Length,total=original+added.Count;
        if(added.Count==0||total>256||rig.Names[0]!="Master"||rig.Parents[0]!=-1)throw new InvalidDataException("Unsupported native rig append population");
        var names=rig.Names.ToHashSet(StringComparer.Ordinal);
        for(int i=0;i<added.Count;i++)if(!names.Add(added[i].Name)||string.IsNullOrEmpty(added[i].Name)||added[i].Name.Length>128||added[i].Name.Any(char.IsControl)||added[i].Parent<0||added[i].Parent>=original+i||added[i].Bind.ParentIndex>=original+i||added[i].Bind.ParentIndex< -1)
            throw new InvalidDataException("Invalid appended bone name/parent/order");
        var edit=new Edit(skeleton);var p=edit.Pack;int obj=p.Object("hkaSkeleton");
        if(p.Array(obj+120,8,16).Count!=0)throw new InvalidDataException("Partitioned native skeleton append is unsupported");
        var bones=p.Array(obj+40,16,256);var parents=p.Array(obj+24,2,256);var reference=p.Array(obj+56,48,256);
        var boneBytes=skeleton.AsSpan(p.Data+bones.At,bones.Count*16).ToArray().Concat(new byte[added.Count*16]).ToArray();
        int newBones=edit.Array(obj+40,total,boneBytes);
        for(int i=0;i<original;i++)edit.Pointer(newBones+i*16,p.Pointer(bones.At+i*16));
        // hkStringPtr reserves bit 0 for heap ownership. Packfile strings are
        // borrowed: an odd fixup target makes the native destructor free text
        // from inside the packfile (436840 -> 429a90 in the native image).
        for(int i=0;i<added.Count;i++){int text=edit.Append(Encoding.UTF8.GetBytes(added[i].Name+'\0'),2);edit.Pointer(newBones+(original+i)*16,text);}
        edit.Array(obj+24,total,skeleton.AsSpan(p.Data+parents.At,parents.Count*2).ToArray().Concat(added.SelectMany(b=>BitConverter.GetBytes(checked((short)b.Parent)))).ToArray());
        edit.Array(obj+56,total,skeleton.AsSpan(p.Data+reference.At,reference.Count*48).ToArray().Concat(added.SelectMany(b=>Transform(b.ReferenceLocal))).ToArray());
        var newSkeleton=edit.Finish();var parsed=NativeAnimation.Skeleton(newSkeleton,allowMirroredScale:true);
        if(parsed.Names.Length!=total||!parsed.Names.Take(original).SequenceEqual(rig.Names)||!parsed.Local.Take(original).SequenceEqual(rig.Local))throw new InvalidDataException("Native rig append changed original reference bones");
        var model=FLVER2.Read(nativeModel);var world=SkinRetarget.World(model.Nodes);var indices=rig.Names.Select(n=>model.Nodes.FindIndex(b=>b.Name==n)).ToArray();
        if(indices.Any(i=>i<0)||indices.Distinct().Count()!=original)throw new InvalidDataException("Native bind rig does not contain unique playback bones");
        var remap=indices.Select((old,index)=>(old,index)).ToDictionary(p=>p.old,p=>p.index);
        short Index(short at)=>at<0?(short)-1:remap.TryGetValue(at,out int n)?checked((short)n):throw new InvalidDataException("Native bind parent is outside the playback rig");
        var output=indices.Select(i=>new FLVER.Node(model.Nodes[i]){Flags=model.Nodes[i].Flags,ParentIndex=Index(model.Nodes[i].ParentIndex),FirstChildIndex=-1,PreviousSiblingIndex=-1,NextSiblingIndex=-1}).ToList();
        output.AddRange(added.Select(b=>new FLVER.Node(b.Bind){Name=b.Name,Flags=b.Bind.Flags,FirstChildIndex=-1,NextSiblingIndex=-1,PreviousSiblingIndex=-1}));
        var newWorld=SkinRetarget.World(output);if(!Matrix4x4.Invert(newWorld[0],out var masterInverse))throw new InvalidDataException("Singular native master bind pose");
        foreach(var dummy in model.Dummies) {
            if(dummy.ParentBoneIndex>=0&&!remap.ContainsKey(dummy.ParentBoneIndex)) {
                var delta=world[dummy.ParentBoneIndex]*masterInverse;dummy.Position=Vector3.Transform(dummy.Position,delta);dummy.Forward=Vector3.TransformNormal(dummy.Forward,delta);dummy.Upward=Vector3.TransformNormal(dummy.Upward,delta);dummy.ParentBoneIndex=0;
            } else if(dummy.ParentBoneIndex>=0)dummy.ParentBoneIndex=checked((short)remap[dummy.ParentBoneIndex]);
            if(dummy.AttachBoneIndex>=0)dummy.AttachBoneIndex=checked((short)remap.GetValueOrDefault(dummy.AttachBoneIndex,0));
        }
        model.Nodes=output;
        for(int i=0;i<output.Count;i++) {
            int parent=output[i].ParentIndex;if(parent<0)continue;var siblings=Enumerable.Range(0,i).Where(j=>output[j].ParentIndex==parent).ToArray();
            if(siblings.Length==0)output[parent].FirstChildIndex=checked((short)i);else{int last=siblings[^1];output[last].NextSiblingIndex=checked((short)i);output[i].PreviousSiblingIndex=checked((short)last);}
        }
        return new(model.Write(),newSkeleton,rig,added.ToArray());
    }
    internal static void ValidateCharacter(byte[] character) {
        var p=new NativeAnimation.Pack(character);int obj=p.Object("hkbCharacterData");
        // Empty LOD/pair arrays select the complete skeleton and do not index
        // an old-sized mirror table. Other contracts require their own adapter.
        if(p.Array(obj+128,4,16).Count!=0||p.Array(p.Object("hkbMirroredSkeletonInfo")+32,2,256).Count!=0)
            throw new InvalidDataException("Native character has a fixed-size bone LOD/mirror contract");
    }
    internal static byte[] Clip(byte[] input,Plan plan,Func<float,IReadOnlyList<Matrix4x4>> sample,bool preserveAddedRootWorld=false) {
        var edit=new Edit(input);var p=edit.Pack;int a=p.Object("hkaSplineCompressedAnimation"),b=p.Object("hkaAnimationBinding"),n=plan.Original.Names.Length;
        int frames=p.Int(a+56),blocks=p.Int(a+60),blockFrames=p.Int(a+64),tracks=p.Int(a+20);float dt=p.Float(a+80);
        int blend=input[p.Data+b+80];
        if(tracks!=n||p.Int(a+24)!=0||p.Int(a+68)!=n*4||blockFrames!=256||frames<2||frames>65536||blocks!=(frames-2)/255+1||blend>1||p.String(b+16)!=plan.Original.Name||p.Array(a+120,4,256).Count!=0||p.Array(a+136,4,256).Count!=0)
            throw new InvalidDataException("Unsupported native spline append contract");
        var binding=p.Array(b+32,2,256);if(binding.Count!=n||Enumerable.Range(0,n).Any(i=>p.Short(binding.At+i*2)!=i))throw new InvalidDataException("Native bone bindings are not identity ordered");
        var data=p.Array(a+152,1,64*1024*1024);var starts=p.Array(a+88,4,256);var ends=p.Array(a+104,4,256);
        if(starts.Count!=blocks||ends.Count!=blocks||p.Int(starts.At)!=0)throw new InvalidDataException("Invalid native spline block table");
        using var stream=new MemoryStream();var newStarts=new List<int>();var newEnds=new List<int>();
        for(int block=0;block<blocks;block++) {
            int start=p.Int(starts.At+block*4),end=block+1<blocks?p.Int(starts.At+(block+1)*4):data.Count,used=p.Int(ends.At+block*4),count=Math.Min(256,frames-block*255);
            if(start<0||start%16!=0||end<=start||end>data.Count||used<n*4||used>end-start||used%4!=0)throw new InvalidDataException("Invalid native spline payload bounds");
            var poses=new List<Matrix4x4>(count*plan.Added.Count);
            var root=preserveAddedRootWorld&&blend==0?SourceAnimation.NativeRoot(input.AsMemory(p.Data+data.At+start,end-start),n,count-1):null;
            if(preserveAddedRootWorld&&(plan.Added[0].Parent!=0||plan.Added.Skip(1).Any(bone=>bone.Parent<n)))throw new InvalidDataException("Absolute source motion requires one appended root attached to native Master");
            for(int frame=0;frame<count;frame++) {
                var values=blend==1?Enumerable.Repeat(Matrix4x4.Identity,plan.Added.Count).ToArray():sample((block*255f+frame)/(frames-1));
                if(values.Count!=plan.Added.Count)throw new InvalidDataException("Appended animation sample count disagrees with rig");
                if(root!=null) {
                    if(!Matrix4x4.Invert(root(frame),out var inverse))throw new InvalidDataException("Singular native animation root");
                    var adjusted=values.ToArray();adjusted[0]*=plan.Original.Local[0]*inverse;values=adjusted;
                }
                poses.AddRange(values);
            }
            var encoded=NativeSpline.Encode(poses,plan.Added.Count,count,dt,plan.Original.Name,plan.Added.Select(bone=>bone.Name).ToArray(),allowMirroredScale:true);
            var extra=new NativeAnimation.Pack(encoded);int ea=extra.Object("hkaSplineCompressedAnimation");var ed=extra.Array(ea+152,1,64*1024*1024);int eEnd=extra.Int(extra.Array(ea+104,4,256).At),mask=plan.Added.Count*4;
            newStarts.Add(checked((int)stream.Length));stream.Write(input.AsSpan(p.Data+data.At+start,n*4));stream.Write(encoded.AsSpan(extra.Data+ed.At,mask));
            stream.Write(input.AsSpan(p.Data+data.At+start+n*4,used-n*4));stream.Write(encoded.AsSpan(extra.Data+ed.At+mask,eEnd-mask));
            newEnds.Add(checked((int)stream.Length-newStarts[^1]));while(stream.Length%16!=0)stream.WriteByte(0);
        }
        edit.Int(a+20,plan.Count);edit.Int(a+68,plan.Count*4);
        edit.Array(a+152,checked((int)stream.Length),stream.ToArray());edit.Array(a+88,blocks,newStarts.SelectMany(BitConverter.GetBytes).ToArray());edit.Array(a+104,blocks,newEnds.SelectMany(BitConverter.GetBytes).ToArray());
        edit.Array(b+32,plan.Count,Enumerable.Range(0,plan.Count).SelectMany(i=>BitConverter.GetBytes(checked((short)i))).ToArray());return edit.Finish();
    }
}

// Reuse the bounded spline reader for the original locomotion root only. The
// returned function does not rewrite or re-encode any native body track.
internal sealed partial class SourceAnimation {
    internal static Func<float,Matrix4x4> NativeRoot(ReadOnlyMemory<byte> bytes,int tracks,int lastFrame) {
        var decoded=ReadBlock(new Reader(bytes),tracks,lastFrame)[0];
        // Missing compressed channels encode identity components; an animation
        // sampler has no skeleton reference argument. c3130 walk omits root Z
        // while its bind root has Z=.093688, so bind fallback would shift it.
        return frame=>Matrix(decoded.Position.Value(frame,Vector3.Zero),decoded.Rotation?.Value(frame)??Quaternion.Identity,decoded.Scale.Value(frame,Vector3.One));
    }
}

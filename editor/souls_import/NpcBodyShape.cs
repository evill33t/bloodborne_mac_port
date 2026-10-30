// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

// DSR body customization is per-bone skin deformation, independent of the
// skeleton's joint positions. The shipped c0000.bsipwv supplies its limits.
internal static class NpcBodyShape {
    internal sealed record BoneScale(int Node,string Bone,int Control,int Axes,int MinimumPercent,int MaximumPercent,Vector3 Scale);
    internal sealed record Report(string SourceFile,string SHA256,IReadOnlyList<int> Controls,IReadOnlyList<BoneScale> Bones,string Limitation);
    internal const string SourceFile="chr/c0000.chrbnd.dcx::c0000.bsipwv";
    internal const string Approximation="Source body-shape controls and per-bone BSI limits are applied. Rigid head/hair dimensions are exact; static baking of multi-bone vertices approximates source runtime deformation (verified initial-idle maximum 8.3mm before native retargeting).";
    static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z);

    internal static Report Apply(FLVER2 model,byte[] boneScaleInfo,IReadOnlyList<int> controls) {
        if(controls.Count!=5||controls.Any(c=>c is <-100 or >100))throw new InvalidDataException("Source body shape requires five signed controls within -100..100");
        if(boneScaleInfo.Length<16||BitConverter.ToUInt32(boneScaleInfo)!=1||boneScaleInfo.AsSpan(4,2).IndexOfAnyExcept((byte)0)>=0||
            boneScaleInfo.AsSpan(8,8).IndexOfAnyExcept((byte)0)>=0)throw new InvalidDataException("Unsupported source body-scale header");
        int count=BitConverter.ToUInt16(boneScaleInfo,6);
        if(count!=model.Nodes.Count||count is <1 or >256||boneScaleInfo.Length!=16+count*4)throw new InvalidDataException("Source body-scale metadata does not match the canonical skeleton");
        var world=SkinRetarget.World(model.Nodes);var transforms=new Matrix4x4[count];var normals=new Matrix4x4[count];var changed=new bool[count];var bindings=new List<BoneScale>();
        for(int n=0;n<count;n++) {
            int at=16+n*4,low=boneScaleInfo[at],high=boneScaleInfo[at+1],flags=boneScaleInfo[at+2],group=flags>>3,axes=flags&7;
            if(boneScaleInfo[at+3]!=0)throw new InvalidDataException("Unsupported source body-scale record padding");
            var scale=Vector3.One;
            if(group<5&&axes!=0) {
                if(low>100||high<100||high>227)throw new InvalidDataException("Unsupported source body-scale percentage limits");
                // DSR a45aaa36...98d7b:31d250 decodes control*0.01;
                // 22a740 decodes BSI percent-100;207f10 computes1+control*
                // 0.01*the signed endpoint delta, then selects local axes.
                float control=controls[group]*.01f;
                float factor=1+control*.01f*(control>=0?high-100:100-low);
                if(!float.IsFinite(factor)||factor<=0)throw new InvalidDataException("Singular source body-scale transform");
                scale=new((axes&1)!=0?factor:1,(axes&2)!=0?factor:1,(axes&4)!=0?factor:1);
                bindings.Add(new(n,model.Nodes[n].Name,group,axes,low,high,scale));
            }
            changed[n]=scale!=Vector3.One;
            if(!Matrix4x4.Invert(world[n],out var inverse))throw new InvalidDataException("Singular source body-scale bind");
            transforms[n]=changed[n]?inverse*Matrix4x4.CreateScale(scale)*world[n]:Matrix4x4.Identity;
            if(!Matrix4x4.Invert(transforms[n],out inverse))throw new InvalidDataException("Singular source body-scale normal transform");
            normals[n]=Matrix4x4.Transpose(inverse);
        }
        // Validate and stage every vertex before mutating the appearance. Bone
        // binds, weights, topology, material masks and animations remain intact.
        var staged=new List<(FLVER.Vertex Vertex,Vector3 Position,Vector3 Normal,Vector4[] Tangents)>();
        foreach(var mesh in model.Meshes) {
            if(!mesh.UseBoneWeights)throw new InvalidDataException("Body shape requires a canonical weighted appearance");
            foreach(var v in mesh.Vertices) {
                if(!Finite(v.Position)||!Finite(v.Normal)||v.Tangents.Any(t=>!Finite(new(t.X,t.Y,t.Z))||!float.IsFinite(t.W)))throw new InvalidDataException("Nonfinite source body-scale vertex");
                var position=Vector3.Zero;var normal=Vector3.Zero;var tangents=new Vector3[v.Tangents.Count];float sum=0;bool affected=false;
                for(int j=0;j<4;j++) {
                    float weight=v.BoneWeights[j];if(!float.IsFinite(weight)||weight<0)throw new InvalidDataException("Invalid source body-scale weight");if(weight==0)continue;
                    int n=SkinRetarget.Bone(mesh,v.BoneIndices[j]);if(n<0||n>=count)throw new InvalidDataException("Invalid source body-scale bone");
                    affected|=changed[n];sum+=weight;position+=Vector3.Transform(v.Position,transforms[n])*weight;normal+=Vector3.TransformNormal(v.Normal,normals[n])*weight;
                    for(int t=0;t<tangents.Length;t++)tangents[t]+=Vector3.TransformNormal(new(v.Tangents[t].X,v.Tangents[t].Y,v.Tangents[t].Z),transforms[n])*weight;
                }
                if(Math.Abs(sum-1)>.001f)throw new InvalidDataException("Source body-scale weights are not normalized");
                if(!affected)continue;
                position/=sum;if(!Finite(position)||!Finite(normal)||normal.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid source body-scale deformation");normal=Vector3.Normalize(normal);
                var outputTangents=new Vector4[tangents.Length];
                for(int t=0;t<tangents.Length;t++) {
                    var direction=tangents[t]-normal*Vector3.Dot(normal,tangents[t]);
                    if(!Finite(direction)||direction.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid source body-scale tangent");
                    outputTangents[t]=new(Vector3.Normalize(direction),v.Tangents[t].W);
                }
                staged.Add((v,position,normal,outputTangents));
            }
        }
        foreach(var entry in staged){entry.Vertex.Position=entry.Position;entry.Vertex.Normal=entry.Normal;entry.Vertex.Tangents.Clear();entry.Vertex.Tangents.AddRange(entry.Tangents);}
        foreach(var mesh in model.Meshes.Where(m=>m.Vertices.Count>0))mesh.BoundingBox=new(){Min=mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min),Max=mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max)};
        if(model.Meshes.Any(m=>m.Vertices.Count>0)) {
            model.Header.BoundingBoxMin=model.Meshes.SelectMany(m=>m.Vertices).Select(v=>v.Position).Aggregate(Vector3.Min);
            model.Header.BoundingBoxMax=model.Meshes.SelectMany(m=>m.Vertices).Select(v=>v.Position).Aggregate(Vector3.Max);
        }
        return new(SourceFile,Convert.ToHexString(SHA256.HashData(boneScaleInfo)).ToLowerInvariant(),controls.ToArray(),bindings,Approximation);
    }
}

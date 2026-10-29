// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

internal sealed class StaticMapGeometry {
    readonly string profile;
    readonly Matrix4x4[] worlds;
    readonly Dictionary<int,Matrix4x4> normalMatrices=[];

    internal StaticMapGeometry(IFlver source,string profile) {
        this.profile=profile;
        worlds=source.Nodes.Count==0?[]:SkinRetarget.World(source.Nodes);
    }

    int RigidBone(IFlverMesh mesh,FLVER.Vertex vertex) {
        if(mesh.Dynamic!=0)return -1; // Weighted vertices are already in bind pose.
        if(profile is "DS1" or "DS1R") {
            int local=vertex.BoneIndices[0];
            int count=mesh is FLVER2.Mesh f?f.BoneIndices.Count:((FLVER0.Mesh)mesh).BoneIndices.Length;
            if(count==0)return -1;
            if(local<0||local>=count)throw new InvalidDataException("Invalid DS1 rigid bone palette index");
            int bone=mesh is FLVER2.Mesh f2?f2.BoneIndices[local]:((FLVER0.Mesh)mesh).BoneIndices[local];
            if(bone < -1 || bone>=worlds.Length)throw new InvalidDataException("Invalid DS1 rigid bone");
            return bone;
        }
        if(mesh is FLVER2.Mesh modern && vertex.NormalW>=0 && vertex.NormalW<modern.BoneIndices.Count) {
            int bone=modern.BoneIndices[vertex.NormalW];
            if(bone<0||bone>=worlds.Length)throw new InvalidDataException("Invalid rigid bone");
            return bone;
        }
        return -1;
    }

    internal FLVER.Vertex Bake(IFlverMesh mesh,FLVER.Vertex input,bool preserveColor=false) {
        int bone=RigidBone(mesh,input);
        var transform=bone<0?Matrix4x4.Identity:worlds[bone];
        if(!normalMatrices.TryGetValue(bone,out var normalMatrix)) {
            if(!Matrix4x4.Invert(transform,out var inverse))throw new InvalidDataException("Singular source node");
            normalMatrices[bone]=normalMatrix=Matrix4x4.Transpose(inverse);
        }
        var v=new FLVER.Vertex(input);
        v.Position=Vector3.Transform(input.Position,transform);
        var normal=Vector3.TransformNormal(input.Normal,normalMatrix);
        if(!float.IsFinite(v.Position.X)||!float.IsFinite(v.Position.Y)||!float.IsFinite(v.Position.Z)||
            !float.IsFinite(normal.LengthSquared())||normal.LengthSquared()<1e-12f)
            throw new InvalidDataException("Invalid static vertex");
        v.Normal=Vector3.Normalize(normal);v.NormalW=0;v.BoneIndices=new();v.BoneWeights=new();
        for(int i=0;i<v.Tangents.Count;++i) {
            var t=input.Tangents[i];var xyz=Vector3.TransformNormal(new Vector3(t.X,t.Y,t.Z),transform);
            xyz-=v.Normal*Vector3.Dot(v.Normal,xyz);
            if(xyz.LengthSquared()<1e-12f)xyz=Vector3.Cross(v.Normal,Math.Abs(v.Normal.X)<.9f?Vector3.UnitX:Vector3.UnitY);
            v.Tangents[i]=new(Vector3.Normalize(xyz),t.W);
        }
        while(v.Tangents.Count<2) {
            var tangent=Vector3.Normalize(Vector3.Cross(v.Normal,Math.Abs(v.Normal.X)<.9f?Vector3.UnitX:Vector3.UnitY));
            v.Tangents.Add(new(tangent,1));
        }
        while(v.UVs.Count<2)v.UVs.Add(Vector3.Zero);
        // The native static layout consumes color semantic 1. Effect alpha must
        // reach that slot even when the source has only color semantic 0.
        var color=(preserveColor||profile is "DS1" or "DS1R")&&input.Colors.Count>0?input.Colors[0]:new FLVER.VertexColor(1f,1f,1f,1f);
        // DS1 RGB carries painted shading/tint, independently of the alpha used
        // for material-layer blending. Keep that tint on opaque native meshes;
        // effect/sky cards still retain their authored alpha as before.
        if(!preserveColor)color=new FLVER.VertexColor(1f,color.R,color.G,color.B);
        v.Colors.Clear();v.Colors.Add(color);v.Colors.Add(color);
        return v;
    }

    internal static IEnumerable<(bool Cull,short Unknown,List<int> Indices)> Faces(IFlver source,IFlverMesh mesh) {
        if(mesh is FLVER2.Mesh modern) {
            foreach(var face in modern.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None))
                yield return(face.CullBackfaces,face.Unk06,face.Triangulate(modern.Vertices.Count<65535));
        } else {
            var legacy=(FLVER0.Mesh)mesh;
            yield return(legacy.CullBackfaces,(short)1,legacy.Triangulate(((FLVER0)source).Header.Version,false,true));
        }
    }
}

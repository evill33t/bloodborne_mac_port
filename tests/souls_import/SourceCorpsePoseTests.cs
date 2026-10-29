// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

static class SourceCorpsePoseTests {
    static void Check(bool condition,string message){if(!condition)throw new Exception(message);}
    static void Reject(Action action,string message) {
        try{action();}catch(InvalidDataException){return;}catch(NotSupportedException){return;}
        throw new Exception(message);
    }
    static void Near(Vector3 a,Vector3 b,float tolerance,string message)=>Check(Vector3.Distance(a,b)<tolerance,message+$": {a} != {b}");
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    public static void Main(string[] args) {
        string root=args.Single();string path=Path.Combine(root,"obj/o0500.objbnd.dcx");var bytes=File.ReadAllBytes(path);var hash=SHA256.HashData(bytes);
        byte[]? Read(string relative)=>relative=="obj/o0500.objbnd.dcx"?bytes:null;
        var original=BND3.Read(bytes);var source=FLVER2.Read(original.Files.Single(f=>Leaf(f.Name)=="o0500.flver").Bytes);
        // Independently sampled with Smithbox's existing spline decoder. These
        // are source-space authored poses, including the below-origin legs of
        // corpses hanging over ledges (31/42), rather than guessed floor poses.
        (int Animation,Vector3 Min,Vector3 Max)[] expected=[
            (10,new(-.36268055f,-.027359376f,-1.0155808f),new(.6138262f,.22222772f,.7367895f)),
            (20,new(-.39633894f,-.03724022f,-.94493675f),new(.3534298f,.4608296f,.73237526f)),
            (31,new(-.3138523f,-1.0347894f,-.68993795f),new(.2611607f,.13663307f,.2621902f)),
            (42,new(-.3911609f,-1.0347894f,-.9447061f),new(.30860454f,.20770606f,.2621902f)),
            (50,new(-.40976915f,-.23259026f,-.7819453f),new(.46025383f,.46113816f,.33698598f)),
            (81,new(-.44667828f,-.048216425f,-.6431123f),new(.120731615f,.38937593f,.51800865f))
        ];
        var poses=new HashSet<string>();
        foreach(var e in expected) {
            var posed=SourceCorpsePose.Load(Read,"DS1R","o0500",e.Animation);
            Check(posed.Nodes.Count==0&&posed.Dummies.Count==0,"Pose retained source runtime selectors");
            Check(posed.Meshes.Sum(m=>m.Vertices.Count)==2144,"Corpse lost source vertices");
            Near(posed.Header.BoundingBoxMin,e.Min,.00003f,"Authored pose minimum changed");Near(posed.Header.BoundingBoxMax,e.Max,.00003f,"Authored pose maximum changed");
            var geometry=new StaticMapGeometry(posed,"DS1R");
            for(int m=0;m<posed.Meshes.Count;++m) {
                var mesh=posed.Meshes[m];var from=source.Meshes[m];
                Check(!mesh.UseBoneWeights&&mesh.NodeIndex==-1&&mesh.BoneIndices.Count==0,"Corpse retained skin/rigid selectors");
                Check(mesh.FaceSets.Count==from.FaceSets.Count&&mesh.FaceSets.Zip(from.FaceSets).All(p=>p.First.Indices.SequenceEqual(p.Second.Indices)),"Corpse topology changed");
                for(int v=0;v<mesh.Vertices.Count;++v) {
                    var actual=mesh.Vertices[v];var before=from.Vertices[v];
                    Check(actual.UVs.SequenceEqual(before.UVs)&&actual.Colors.SequenceEqual(before.Colors),"Corpse UV/color data changed");
                    Check(actual.NormalW==0&&Enumerable.Range(0,4).All(i=>actual.BoneIndices[i]==0&&actual.BoneWeights[i]==0),"Vertex retained source pose selectors");
                    Check(Math.Abs(actual.Normal.LengthSquared()-1)<.00001f,"Corpse normal is not unit length");
                    foreach(var t in actual.Tangents){var xyz=new Vector3(t.X,t.Y,t.Z);Check(Math.Abs(xyz.LengthSquared()-1)<.00001f&&Math.Abs(Vector3.Dot(xyz,actual.Normal))<.00001f,"Corpse tangent is not orthonormal");}
                    Near(geometry.Bake(mesh,actual).Position,actual.Position,.000001f,"Static conversion applies the corpse pose twice");
                }
            }
            // Exercise the original format's vertex layout readback as well as
            // the independent native static conversion above.
            var readback=FLVER2.Read(posed.Write());
            Near(readback.Header.BoundingBoxMin,e.Min,.00003f,"Serialized pose minimum changed");
            Near(readback.Header.BoundingBoxMax,e.Max,.00003f,"Serialized pose maximum changed");
            Check(readback.Meshes.All(m=>!m.UseBoneWeights&&m.NodeIndex==-1&&m.BoneIndices.Count==0),"Serialized mesh selectors changed");
            for(int m=0;m<posed.Meshes.Count;++m)for(int v=0;v<posed.Meshes[m].Vertices.Count;++v)
                Near(readback.Meshes[m].Vertices[v].Position,posed.Meshes[m].Vertices[v].Position,.000001f,"Serialized posed position changed");
            poses.Add(Convert.ToHexString(SHA256.HashData(posed.Write())));
            Console.WriteLine($"PASS o0500 InitAnimID {e.Animation}: 2144 vertices; bounds {e.Min} .. {e.Max}");
        }
        Check(poses.Count==6,"Per-instance pose identity collapsed");
        int reads=0;byte[]? Unused(string p){++reads;return null;}
        Reject(()=>SourceCorpsePose.Load(Unused,"DS1","o0500",10),"Unverified original DS1 profile accepted");
        Reject(()=>SourceCorpsePose.Load(Unused,"DS1R","o0501",10),"Unverified object accepted");
        Reject(()=>SourceCorpsePose.Load(Unused,"DS1R","o0500",99),"Unverified animation accepted");
        Check(reads==0,"Unsupported input reads source archives");
        void CorruptModel(Action<FLVER2> corrupt,string message) {
            var archive=BND3.Read(bytes);var file=archive.Files.Single(f=>Leaf(f.Name)=="o0500.flver");var model=FLVER2.Read(file.Bytes);corrupt(model);file.Bytes=model.Write();var modified=archive.Write();
            Reject(()=>SourceCorpsePose.Load(_=>modified,"DS1R","o0500",10),message);
        }
        CorruptModel(m=>m.Meshes[0].BoneIndices[0]=999,"Invalid palette node accepted");
        CorruptModel(m=>m.Nodes[14].Name="UnmatchedHead","Missing named skeleton bone accepted");
        CorruptModel(m=>m.Nodes[14].Scale=Vector3.Zero,"Singular bind matrix accepted");
        CorruptModel(m=>m.Meshes[0].Vertices[0].Position=new(float.NaN,0,0),"Nonfinite source position accepted");
        var altered=BND3.Read(bytes);var anifile=altered.Files.Single(f=>Leaf(f.Name)=="o0500.anibnd");var animations=BND3.Read(anifile.Bytes);var clipfile=animations.Files.Single(f=>Leaf(f.Name)=="a00_0010.hkx");
        var clipbytes=clipfile.Bytes.ToArray();var tag=new SourceCollision.Tag(clipbytes);var anim=tag.Items.Single(i=>tag.Name(i.Type)=="hkaSplineCompressedAnimation");clipbytes[tag.Array(anim,"data").At+1]|=0x10;
        clipfile.Bytes=clipbytes;anifile.Bytes=animations.Write();var unsupported=altered.Write();
        Reject(()=>SourceCorpsePose.Load(_=>unsupported,"DS1R","o0500",10),"Spline track accepted by static pose decoder");
        Check(SHA256.HashData(File.ReadAllBytes(path)).SequenceEqual(hash),"Original game object archive changed");
        Console.WriteLine("PASS all six independent pose bounds, palette/name validation, normals/tangents, serialization, no double transform, unsupported/corrupt inputs, and source hash");
    }
}

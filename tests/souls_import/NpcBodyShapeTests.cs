// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class NpcBodyShapeTests {
    static void Check(bool condition,string message){if(!condition)throw new Exception(message);}
    static void Near(Vector3 actual,Vector3 expected,float tolerance,string message)=>Check(Vector3.Distance(actual,expected)<tolerance,$"{message}: {actual} != {expected}");
    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] data)=>Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
    static Matrix4x4[] World(IReadOnlyList<Matrix4x4> local,IReadOnlyList<int> parents) {
        var result=new Matrix4x4[local.Count];var ready=new bool[result.Length];
        Matrix4x4 At(int i){if(ready[i])return result[i];ready[i]=true;return result[i]=local[i]*(parents[i]<0?Matrix4x4.Identity:At(parents[i]));}
        for(int i=0;i<result.Length;i++)At(i);return result;
    }
    static Matrix4x4[] Inverse(IReadOnlyList<Matrix4x4> world)=>world.Select(m=>{Check(Matrix4x4.Invert(m,out var inverse),"Source bone bind is singular");return inverse;}).ToArray();
    static Matrix4x4[] Bind(FLVER2 model)=>World(model.Nodes.Select(n=>n.ComputeLocalTransform()).ToArray(),model.Nodes.Select(n=>(int)n.ParentIndex).ToArray());
    static int Bone(FLVER2.Mesh mesh,int index)=>mesh.BoneIndices.Count==0?index:mesh.BoneIndices[index];
    // Independent evaluation of the source parser/consumer: metadata stores
    // percentage endpoints and a control/axis byte, not direct scale factors.
    static Vector3[] SourceScales(byte[] metadata,IReadOnlyList<int> controls,int count) {
        var result=new Vector3[count];
        for(int i=0;i<count;i++) {
            int at=16+4*i,flags=metadata[at+2],group=flags>>3;double factor=1;
            if(group<5) {
                int control=controls[group];int endpoint=control>=0?metadata[at+1]-100:100-metadata[at];
                factor=1+control*endpoint/10000.0;
            }
            result[i]=new((flags&1)!=0?(float)factor:1,(flags&2)!=0?(float)factor:1,(flags&4)!=0?(float)factor:1);
        }
        return result;
    }
    static Vector3 Weighted(FLVER2.Mesh mesh,FLVER.Vertex vertex,IReadOnlyList<Matrix4x4> matrices) {
        var result=Vector3.Zero;float total=0;
        for(int i=0;i<4;i++)if(vertex.BoneWeights[i]>0){float w=vertex.BoneWeights[i];result+=Vector3.Transform(vertex.Position,matrices[Bone(mesh,vertex.BoneIndices[i])])*w;total+=w;}
        Check(Math.Abs(total-1)<.0001f,"Source skin weights are not normalized");return result/total;
    }
    static Matrix4x4[] Pose(FLVER2 model,SourceAnimation clip,float time) {
        var world=World(clip.Sample(time),clip.Bones.Select(b=>b.Parent).ToArray());
        var byName=clip.Bones.Select((b,i)=>(b.Name,i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
        var result=new Matrix4x4[model.Nodes.Count];var ready=new bool[result.Length];
        Matrix4x4 At(int i){if(ready[i])return result[i];ready[i]=true;return result[i]=byName.TryGetValue(model.Nodes[i].Name,out int n)?world[n]:model.Nodes[i].ComputeLocalTransform()*(model.Nodes[i].ParentIndex<0?Matrix4x4.Identity:At(model.Nodes[i].ParentIndex));}
        for(int i=0;i<result.Length;i++)At(i);return result;
    }
    static void Preserved(FLVER2 before,FLVER2 after) {
        // Restore only the documented mutable geometry and bounds, then require
        // complete serialized equality to catch node, weight or draw-data loss.
        var copy=FLVER2.Read(after.Write());
        copy.Header.BoundingBoxMin=before.Header.BoundingBoxMin;copy.Header.BoundingBoxMax=before.Header.BoundingBoxMax;
        Check(before.Meshes.Count==copy.Meshes.Count&&before.Nodes.Count==copy.Nodes.Count,"Body shaping changed the skeleton or mesh count");
        for(int m=0;m<before.Meshes.Count;m++) {
            var a=before.Meshes[m];var b=copy.Meshes[m];b.BoundingBox=a.BoundingBox;
            Check(a.Vertices.Count==b.Vertices.Count,"Body shaping changed source vertex count");
            for(int v=0;v<a.Vertices.Count;v++) {
                b.Vertices[v].Position=a.Vertices[v].Position;b.Vertices[v].Normal=a.Vertices[v].Normal;
                b.Vertices[v].Tangents=a.Vertices[v].Tangents.ToList();
            }
        }
        Check(before.Write().SequenceEqual(copy.Write()),"Body shaping changed nodes, weights, UVs, colors, topology, materials or other source metadata");
    }
    static void RejectUnchanged(FLVER2 original,byte[] metadata,IReadOnlyList<int> controls,string reason,Action<FLVER2>? corrupt=null) {
        var model=FLVER2.Read(original.Write());corrupt?.Invoke(model);var before=model.Write();var input=metadata.ToArray();var sliders=controls.ToArray();
        bool rejected=false;
        try{NpcBodyShape.Apply(model,metadata,controls);}catch(Exception e)when(e is ArgumentException or InvalidDataException or NotSupportedException){rejected=true;}
        Check(rejected,"Invalid body shape accepted: "+reason);
        Check(before.SequenceEqual(model.Write()),"Rejected body shape partially mutated the model: "+reason);
        Check(input.SequenceEqual(metadata)&&sliders.SequenceEqual(controls),"Body shape mutated metadata/controls: "+reason);
    }
    static void Bounds(FLVER2 model) {
        var points=model.Meshes.SelectMany(m=>m.Vertices).Select(v=>v.Position).ToArray();
        Near(model.Header.BoundingBoxMin,points.Aggregate(Vector3.Min),.000001f,"Body shape minimum bounds are stale");
        Near(model.Header.BoundingBoxMax,points.Aggregate(Vector3.Max),.000001f,"Body shape maximum bounds are stale");
        foreach(var mesh in model.Meshes)if(mesh.BoundingBox!=null) {
            var vertices=mesh.Vertices.Select(v=>v.Position).ToArray();
            Near(mesh.BoundingBox.Min,vertices.Aggregate(Vector3.Min),.000001f,"Body shape mesh minimum bounds are stale");
            Near(mesh.BoundingBox.Max,vertices.Aggregate(Vector3.Max),.000001f,"Body shape mesh maximum bounds are stale");
        }
    }
    public static void Main(string[] args) {
        if(args.Length!=2)throw new ArgumentException("Supply the DS1R game root and DS1R parameter definitions");
        string root=args[0],defs=args[1];var hashes=new Dictionary<string,string>();
        byte[]? Read(string relative){string path=Path.Combine(root,relative);if(!File.Exists(path))return null;var data=File.ReadAllBytes(path);hashes.TryAdd(path,Hash(data));return data;}
        var main=BND3.Read(Read("chr/c0000.chrbnd.dcx")!);
        var bsiEntry=main.Files.Single(f=>Leaf(f.Name)=="c0000.bsipwv");var bsi=bsiEntry.Bytes.ToArray();var bsiCopy=bsi.ToArray();
        Check(bsiEntry.ID==600&&bsi.Length==336&&BitConverter.ToUInt16(bsi,6)==80,"Actual source c0000 body-scale metadata contract changed");
        Check(Hash(bsi)=="dea7aafcb7e849807c1af9ababd99a4c6e2b9d43fc89d92659246ceaa207a635","Source BSI fixture changed; re-audit runtime percentage interpretation");
        Check(bsi.AsSpan(16+19*4,4).SequenceEqual(new byte[]{95,110,7,0})&&bsi.AsSpan(16+18*4,4).SequenceEqual(new byte[]{90,120,6,0}),"Authored head/neck percentage or local-axis contract changed");
        var bank=BND3.Read(Read("param/GameParam/GameParam.parambnd.dcx")!);
        var chara=PARAM.Read(bank.Files.Single(f=>Leaf(f.Name)=="CharaInitParam.param").Bytes);chara.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"CharaInitParam.xml")));
        var skeleton=BND3.Read(Read("chr/c0000.anibnd.dcx")!).Files.Single(f=>Leaf(f.Name).Equals("Skeleton.hkx",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
        var reports=new List<object>();FLVER2? fixture=null;
        foreach(int id in new[]{6270,6080}) {
            var row=chara.Rows.Single(r=>r.ID==id);var controls=new[]{"bodyScaleHead","bodyScaleBreast","bodyScaleAbdomen","bodyScaleArm","bodyScaleLeg"}.Select(n=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName==n).Value)).ToArray();
            Check(controls.SequenceEqual(id==6080?new[]{100,-30,50,-40,60}:new[]{40,-30,20,-30,-30}),"Actual CharaInit customization fixture changed");
            var appearance=NpcAppearance.Load(Read,defs,id);Check(appearance.BodyShape==null,"Default legacy NPC assembly unexpectedly applies body shaping");
            var original=appearance.Model;var unchanged=original.Write();var shaped=FLVER2.Read(unchanged);fixture??=original;
            var report=NpcBodyShape.Apply(shaped,bsi,controls);
            Check(report.SHA256==Hash(bsi)&&report.Controls.SequenceEqual(controls)&&report.Bones.Count==29,"Body-shape report lacks exact source metadata and controls");
            Check(report.SourceFile=="chr/c0000.chrbnd.dcx::c0000.bsipwv","Body-shape report omitted source binder provenance");
            Check(report.Limitation.Contains("multi-bone",StringComparison.OrdinalIgnoreCase),"Static multi-bone approximation is not explicitly reported");
            var bind=Bind(original);var inverse=Inverse(bind);var scales=SourceScales(bsi,controls,bind.Length);
            Near(scales[19],new(id==6080?1.10f:1.04f),.000001f,"Runtime Head factor is incorrect");
            Near(scales[18],new(1,id==6080?1.20f:1.08f,id==6080?1.20f:1.08f),.000001f,"Runtime Neck local-axis factor is incorrect");
            Near(report.Bones.Single(b=>b.Node==19).Scale,scales[19],.000001f,"Head report has wrong customization factor");
            Near(report.Bones.Single(b=>b.Node==18).Scale,scales[18],.000001f,"Neck report has wrong local-axis factor");
            foreach(var bone in report.Bones) {
                int at=16+bone.Node*4;
                Check(bone.Bone==original.Nodes[bone.Node].Name&&bone.Control==bsi[at+2]>>3&&bone.Axes==(bsi[at+2]&7)&&bone.MinimumPercent==bsi[at]&&bone.MaximumPercent==bsi[at+1],"Body-shape report changed a source bone/metadata binding");
                Near(bone.Scale,scales[bone.Node],.000001f,"A source body-scale percentage or local axis was misinterpreted");
            }
            var deformation=Enumerable.Range(0,bind.Length).Select(n=>inverse[n]*Matrix4x4.CreateScale(scales[n])*bind[n]).ToArray();
            var normalTransforms=Inverse(deformation).Select(Matrix4x4.Transpose).ToArray();
            int changed=0,rigidHead=0;
            for(int m=0;m<original.Meshes.Count;m++)for(int v=0;v<original.Meshes[m].Vertices.Count;v++) {
                var before=original.Meshes[m].Vertices[v];var after=shaped.Meshes[m].Vertices[v];
                Near(after.Position,Weighted(original.Meshes[m],before,deformation),.000003f,"Static body shape differs from independent source BSI formula");
                var expectedNormal=Vector3.Zero;
                for(int j=0;j<4;j++)if(before.BoneWeights[j]>0)expectedNormal+=Vector3.TransformNormal(before.Normal,normalTransforms[Bone(original.Meshes[m],before.BoneIndices[j])])*before.BoneWeights[j];
                Near(after.Normal,Vector3.Normalize(expectedNormal),.00001f,"Body shape normal differs from independent inverse-transpose deformation");
                if(Vector3.Distance(before.Position,after.Position)>.000001f)++changed;
                if(before.BoneWeights[0]>.99999f&&original.Nodes[Bone(original.Meshes[m],before.BoneIndices[0])].Name=="Head")++rigidHead;
                Check(float.IsFinite(after.Normal.LengthSquared())&&Math.Abs(after.Normal.LengthSquared()-1)<.0002f,"Body shape produced a nonunit normal");
                for(int t=0;t<after.Tangents.Count;t++) {
                    var tangent=after.Tangents[t];var direction=new Vector3(tangent.X,tangent.Y,tangent.Z);
                    var beforeTangent=before.Tangents[t];var expectedTangent=Vector3.Zero;
                    for(int j=0;j<4;j++)if(before.BoneWeights[j]>0)expectedTangent+=Vector3.TransformNormal(new(beforeTangent.X,beforeTangent.Y,beforeTangent.Z),deformation[Bone(original.Meshes[m],before.BoneIndices[j])])*before.BoneWeights[j];
                    expectedTangent=Vector3.Normalize(expectedTangent-after.Normal*Vector3.Dot(after.Normal,expectedTangent));
                    Near(direction,expectedTangent,.00001f,"Body shape tangent differs from independent deformation");
                    Check(float.IsFinite(direction.LengthSquared())&&Math.Abs(direction.LengthSquared()-1)<.0002f&&Math.Abs(Vector3.Dot(direction,after.Normal))<.0002f,"Body-shape tangent is invalid or not perpendicular");
                    Check(tangent.W==before.Tangents[t].W,"Body shape changed tangent handedness");
                }
            }
            Check(changed>5000&&rigidHead>1000,$"Fixture did not exercise actual source body and rigid face/hair geometry: NPC{id}, changed{changed}, head{rigidHead}");Bounds(shaped);Preserved(original,shaped);
            var integrated=NpcAppearance.Load(Read,defs,id,applyBodyShape:true);
            Check(integrated.BodyShape!=null&&integrated.BodyShape.Controls.SequenceEqual(controls),"NPC assembly failed to apply the source body shape controls");
            // The isolated helper fixture crosses a real FLVER write/read first;
            // direct assembly retains unquantized source weights until export.
            float integrationError=0;
            for(int m=0;m<shaped.Meshes.Count;m++)for(int v=0;v<shaped.Meshes[m].Vertices.Count;v++) {
                var a=integrated.Model.Meshes[m].Vertices[v];var b=shaped.Meshes[m].Vertices[v];integrationError=Math.Max(integrationError,Vector3.Distance(a.Position,b.Position));
                Near(a.Position,b.Position,.00002f,"NPC integrated body shape differs beyond source-weight serialization precision");
            }
            Check(unchanged.SequenceEqual(original.Write()),"Body-shape test mutated original source appearance");
            string archive=id==6270?"chr/c0000_a00_md.anibnd.dcx":"chr/c0000_a00_lo.anibnd.dcx";
            int clipID=id==6270?7810:0;var animation=BND3.Read(Read(archive)!);var clip=SourceAnimation.Decode(skeleton,animation.Files.Single(f=>Leaf(f.Name)==$"a00_{clipID:D4}.hkx").Bytes.ToArray());
            var frames=new List<object>();
            foreach(int frame in new[]{0,clip.FrameCount/2,clip.FrameCount-1}) {
                var pose=Pose(original,clip,frame*clip.FrameDuration);
                var exact=Enumerable.Range(0,bind.Length).Select(n=>inverse[n]*Matrix4x4.CreateScale(scales[n])*pose[n]).ToArray();
                var ordinary=Enumerable.Range(0,bind.Length).Select(n=>inverse[n]*pose[n]).ToArray();
                var errors=new List<float>();var headErrors=new List<float>();
                for(int m=0;m<original.Meshes.Count;m++)for(int v=0;v<original.Meshes[m].Vertices.Count;v++) {
                    var before=original.Meshes[m].Vertices[v];var after=shaped.Meshes[m].Vertices[v];float error=Vector3.Distance(Weighted(original.Meshes[m],before,exact),Weighted(shaped.Meshes[m],after,ordinary));errors.Add(error);
                    if(before.BoneWeights[0]>.99999f&&original.Nodes[Bone(original.Meshes[m],before.BoneIndices[0])].Name=="Head")headErrors.Add(error);
                }
                Check(errors.Max()<(id==6270?.0083f:.0047f)&&errors.Average()<.0003f,"Static blend-shape approximation exceeded the audited idle error budget");
                Check(headErrors.Count>1000&&headErrors.Max()<.000003f,"Rigid head scale is not exact under source animation");
                frames.Add(new{frame,maximumError=errors.Max(),meanError=errors.Average(),rigidHeadMaximumError=headErrors.Max()});
            }
            var reopened=FLVER2.Read(shaped.Write());Bounds(reopened);Preserved(original,reopened);reports.Add(new{id,report,changed,rigidHead,integrationError,frames});
        }
        var valid=fixture!;var sliders=new[]{100,-30,50,-40,60};
        foreach(int value in new[]{-100,0,100}) {
            var model=FLVER2.Read(valid.Write());int[] controls=[value,value,value,value,value];var expected=SourceScales(bsi,controls,model.Nodes.Count);
            var result=NpcBodyShape.Apply(model,bsi,controls);
            foreach(var bone in result.Bones)Near(bone.Scale,expected[bone.Node],.000001f,"Signed control endpoint changed");
            if(value==0)for(int m=0;m<valid.Meshes.Count;m++)for(int v=0;v<valid.Meshes[m].Vertices.Count;v++) {
                var a=valid.Meshes[m].Vertices[v];var b=model.Meshes[m].Vertices[v];
                Check(a.Position==b.Position&&a.Normal==b.Normal&&a.Tangents.SequenceEqual(b.Tangents),"Zero controls altered source geometry");
            }
        }
        // The engine disables out-of-range groups or records with no axes;
        // unsupported inactive endpoints must not become a guessed deformation.
        foreach(byte flags in new byte[]{0,255}) {
            var metadata=bsi.ToArray();metadata[16+19*4]=255;metadata[17+19*4]=0;metadata[18+19*4]=flags;
            var model=FLVER2.Read(valid.Write());var result=NpcBodyShape.Apply(model,metadata,sliders);
            Check(result.Bones.Count==28&&result.Bones.All(b=>b.Node!=19),"Disabled BSI group/axes were not skipped like the source runtime");
        }
        RejectUnchanged(valid,bsi,sliders[..4],"missing control");RejectUnchanged(valid,bsi,[..sliders,0],"extra control");
        RejectUnchanged(valid,bsi,[101,-30,50,-40,60],"head above source range");RejectUnchanged(valid,bsi,[-101,-30,50,-40,60],"head below source range");
        RejectUnchanged(valid,bsi[..^1],sliders,"truncated record");RejectUnchanged(valid,[..bsi,0],sliders,"trailing metadata");
        void Metadata(int at,byte value,string reason){var bad=bsi.ToArray();bad[at]=value;RejectUnchanged(valid,bad,sliders,reason);}
        Metadata(0,2,"unsupported BSI version");Metadata(6,79,"mismatched node count");Metadata(16+19*4+3,1,"reserved record byte");
        Metadata(4,1,"reserved header byte");Metadata(8,1,"reserved header word");
        Metadata(16+19*4,200,"invalid minimum percent");Metadata(16+19*4+1,90,"maximum percent below identity");
        var singular=bsi.ToArray();singular[16+19*4]=0;RejectUnchanged(valid,singular,[-100,0,0,0,0],"singular scale factor");
        RejectUnchanged(valid,bsi,sliders,"singular bind",m=>m.Nodes[19].Scale=Vector3.Zero);
        RejectUnchanged(valid,bsi,sliders,"unknown weighted node",m=>m.Meshes[^1].Vertices[^1].BoneIndices[0]=200);
        RejectUnchanged(valid,bsi,sliders,"negative weight",m=>m.Meshes[^1].Vertices[^1].BoneWeights[0]=-.5f);
        RejectUnchanged(valid,bsi,sliders,"unnormalized weight",m=>m.Meshes[^1].Vertices[^1].BoneWeights[0]=2);
        RejectUnchanged(valid,bsi,sliders,"nonfinite weight",m=>m.Meshes[^1].Vertices[^1].BoneWeights[0]=float.NaN);
        RejectUnchanged(valid,bsi,sliders,"nonfinite last vertex",m=>m.Meshes[^1].Vertices[^1].Position=new(float.NaN,0,0));
        Check(bsiCopy.SequenceEqual(bsi),"Body-shape conversion changed source BSI bytes");
        foreach(var entry in hashes)Check(Hash(File.ReadAllBytes(entry.Key))==entry.Value,"Source asset changed: "+entry.Key);
        string output="out/map-editor/npc-body-shape-tests/report.json";Directory.CreateDirectory(Path.GetDirectoryName(output)!);File.WriteAllText(output,JsonSerializer.Serialize(new{reports,sourceHashes=hashes},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine("PASS: source BSI percentages/local axes, Petrus and Crestfallen body shapes, exact animated rigid heads, bounded blended idle error, geometry-only round trip, malformed-input atomicity and source asset hashes");
    }
}

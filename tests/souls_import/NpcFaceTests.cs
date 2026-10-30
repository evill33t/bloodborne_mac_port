// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class NpcFaceTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Near(Vector3 a,Vector3 b,float tolerance,string message)=>Check(Vector3.Distance(a,b)<tolerance,message+$": {a} != {b}");
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static float NativeWinding(FLVER2 model,int meshes) {
        int negative=0,count=0;
        foreach(var mesh in model.Meshes.Take(meshes))foreach(var fs in mesh.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None)) {
            var indices=fs.Triangulate(mesh.Vertices.Count<65535);
            for(int i=0;i<indices.Count;i+=3) {
                var a=mesh.Vertices[indices[i]];var b=mesh.Vertices[indices[i+1]];var c=mesh.Vertices[indices[i+2]];
                var cross=Vector3.Cross(b.Position-a.Position,c.Position-a.Position);var normal=a.Normal+b.Normal+c.Normal;
                if(cross.LengthSquared()<1e-14f||normal.LengthSquared()<1e-8f)continue;
                ++count;if(Vector3.Dot(cross,normal)<0)++negative;
            }
        }
        Check(count>0,"No usable source/native culling reference facets");return (float)negative/count;
    }
    // Find rims from real topology, independently of FaceGen's F2TR mapping.
    // The authored skull has a40-point face opening and16-point neck opening;
    // generated face meshes0+1 have the same40-point rim plus eye sockets.
    static Vector3[] FaceRim(IEnumerable<FLVER2.Mesh> meshes) {
        var indices=new Dictionary<(double,double,double),int>();var points=new List<Vector3>();
        var edges=new Dictionary<(int,int),int>();
        int Point(Vector3 p) {
            var key=(Math.Round(p.X,5),Math.Round(p.Y,5),Math.Round(p.Z,5));
            if(indices.TryGetValue(key,out int n))return n;indices.Add(key,points.Count);points.Add(p);return points.Count-1;
        }
        foreach(var mesh in meshes)foreach(var fs in mesh.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None)) {
            var faces=fs.Triangulate(mesh.Vertices.Count<65535);
            for(int i=0;i<faces.Count;i+=3) {
                var t=Enumerable.Range(0,3).Select(j=>Point(mesh.Vertices[faces[i+j]].Position)).ToArray();
                if(t.Distinct().Count()!=3)continue;
                for(int j=0;j<3;j++){int a=t[j],b=t[(j+1)%3];if(a>b)(a,b)=(b,a);edges[(a,b)]=edges.GetValueOrDefault((a,b))+1;}
            }
        }
        var links=new Dictionary<int,HashSet<int>>();
        foreach(var ((a,b),count) in edges)if(count==1) {
            if(!links.ContainsKey(a))links.Add(a,[]);if(!links.ContainsKey(b))links.Add(b,[]);links[a].Add(b);links[b].Add(a);
        }
        var components=new List<Vector3[]>();var visited=new HashSet<int>();
        foreach(int start in links.Keys) {
            if(visited.Contains(start))continue;var queue=new Queue<int>();var component=new List<Vector3>();queue.Enqueue(start);visited.Add(start);
            while(queue.TryDequeue(out int at)){component.Add(points[at]);foreach(int next in links[at])if(visited.Add(next))queue.Enqueue(next);}
            components.Add(component.ToArray());
        }
        return components.Single(c=>c.Length==40);
    }
    static void Reject(Action action) {try{action();}catch(InvalidDataException){return;}catch(NotSupportedException){return;}throw new Exception("Invalid face input accepted");}
    public static void Main(string[] args) {
        string root=args[0],native=args[1],defs=args[2];var hashes=new Dictionary<string,byte[]>();
        byte[]? Read(string path) {
            string full=Path.Combine(root,path);if(!File.Exists(full))return null;var bytes=File.ReadAllBytes(full);hashes.TryAdd(full,SHA256.HashData(bytes));return bytes;
        }
        var binder=BND3.Read(Read("param/GameParam/GameParam.parambnd.dcx")!);
        PARAM Param(string name){var p=PARAM.Read(binder.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,name+".xml")));return p;}
        var faces=Param("FaceGenParam");var hair=Param("EquipParamProtector");
        var faceAssets=BND3.Read(Read("facegen/FaceGen.fgbnd")!);
        var faceTemplate=FLVER2.Read(faceAssets.Files.Single(f=>Leaf(f.Name)=="face.flver").Bytes);
        void DrawVariants(FLVER2 model,string stage) {
            for(int m=0;m<3;m++) {
                var actual=model.Meshes[m].FaceSets;var expected=faceTemplate.Meshes[m].FaceSets;
                Check(actual.Count==6&&actual.Count==expected.Count,stage+": face draw variants were omitted");
                for(int f=0;f<expected.Count;f++) {
                    var before=expected[f];var after=actual[f];
                    var winding=before.Triangulate(faceTemplate.Meshes[m].Vertices.Count<65535);
                    Check(after.Flags==before.Flags&&after.CullBackfaces==before.CullBackfaces&&after.Unk06==before.Unk06&&
                        after.Triangulate(model.Meshes[m].Vertices.Count<65535).SequenceEqual(winding),
                        stage+": source face LOD/motion topology changed");
                }
            }
        }
        foreach(int sex in new[]{0,1})for(int style=0;style<20;++style) {
            var row=hair.Rows.Single(r=>r.ID==(sex==1?1000:3000)+100*style);int model=Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="equipModelId").Value);
            Check(NpcFace.HairModel(style,sex)==model,"Hairstyle mapping differs from actual source equip parameter");
        }
        Check(NpcFace.Coefficient(0)==-10&&NpcFace.Coefficient(255)==10,"Saved coefficient endpoints changed");
        Near(new(NpcFace.Coefficient(127),NpcFace.Coefficient(128),0),new(-.03921604f,.03921604f,0),.000002f,"Saved coefficient midpoint changed");
        NpcAppearanceTests.Run();
        var geometryHashes=new HashSet<string>();var textureHashes=new HashSet<string>();var reports=new List<object>();
        foreach(int id in new[]{6080,6270}) {
            var row=faces.Rows.Single(r=>r.ID==id);var result=NpcFace.Load(Read,row,1);
            var standalone=FLVER2.Read(result.Model.Write());
            DrawVariants(standalone,"Generated source readback");
            // Independent shipped geometry establishes the engine's front-face
            // convention, rather than comparing two generated appearances.
            Check(NativeWinding(result.SourceParts[0],result.SourceParts[0].Meshes.Count)>.99f,"Original DS1 skull culling convention changed");
            Check(NativeWinding(standalone,3)>.99f,"Generated face is inside-out against the original source skull");
            var skullRim=FaceRim(result.SourceParts[0].Meshes);var generatedRim=FaceRim(standalone.Meshes.Take(2));
            float seamError=generatedRim.Max(p=>skullRim.Min(q=>Vector3.Distance(p,q)));
            Check(seamError<.000002f,"FaceGen basis does not join the independently authored skull opening: "+seamError);
            for(int m=0;m<result.Model.Meshes.Count;m++)for(int v=0;v<result.Model.Meshes[m].Vertices.Count;v++)for(int uv=0;uv<2;uv++) {
                var delta=Vector3.Abs(standalone.Meshes[m].Vertices[v].UVs[uv]-result.Model.Meshes[m].Vertices[v].UVs[uv]);
                Check(delta.X<=.5001f/1024&&delta.Y<=.5001f/1024,"Standalone generated face UV export scaled or wrapped a source set");
            }
            Check(result.Model.Meshes.Count==3&&result.Model.Meshes.Sum(m=>m.Vertices.Count)==1052,"Generated face source vertex/mesh count changed");
            Check(result.Model.Meshes.SelectMany(m=>m.FaceSets).Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None).Sum(f=>f.Indices.Count/3)==1704,"Main source face/eye triangle count changed");
            Check(result.SourceParts.Count==2&&result.SourcePaths.Count==4,"Face lacks original neck or selected hair part");
            Check(result.SourcePaths.Contains(id==6080?"parts/HR_M_0003.partsbnd.dcx":"parts/HR_M_0011.partsbnd.dcx"),"Source hairstyle resolved incorrectly");
            var expectedMin=id==6080?new Vector3(-.057641346f,1.5042477f,-.13450328f):new Vector3(-.056093f,1.5074661f,-.13956593f);
            var expectedMax=id==6080?new Vector3(.057570063f,1.650441f,-.03846198f):new Vector3(.056092992f,1.650441f,-.03846198f);
            Near(result.Model.Header.BoundingBoxMin,expectedMin,.000001f,"Independent EGM face minimum differs");Near(result.Model.Header.BoundingBoxMax,expectedMax,.000001f,"Independent EGM face maximum differs");
            var generated=result.Textures.Single(t=>t.Name==$"FG_import_{id}_face");var dds=new DDS(generated.Bytes);Check(dds.dwWidth==128&&dds.dwHeight==128,"Generated EGT diffuse dimensions changed");
            // Independent Python fixed-point/float32 reconstruction from the
            // original BMP + all 50 EGT planes, excluding the DDS container.
            string pixelHash=id==6080?"B2BE035C8211924226F83F25F7D2681D8D735CDD5E496CBC5ADB4FAB2C60361D":"508952A6892B21F19C85BFF785BBFB291311217E81CB4BC8844DD7949958AA8D";
            Check(Convert.ToHexString(SHA256.HashData(generated.Bytes.AsSpan(generated.Bytes.Length-128*128*4)))==pixelHash,"Full generated EGT image differs from independent reconstruction");
            textureHashes.Add(Convert.ToHexString(SHA256.HashData(generated.Bytes)));
            int head=result.Model.Nodes.FindIndex(n=>n.Name=="Head");
            foreach(var mesh in result.Model.Meshes)foreach(var v in mesh.Vertices) {
                Check(v.BoneIndices[0]==head&&v.BoneWeights[0]==1&&v.BoneWeights[1]==0,"Generated face does not use verified canonical Head weights");
                Check(float.IsFinite(v.Position.LengthSquared())&&Math.Abs(v.Normal.LengthSquared()-1)<.00001f,"Invalid generated face position/normal");
                Check(v.UVs.Count==2&&v.UVs.All(uv=>float.IsFinite(uv.LengthSquared()))&&mesh.Vertices.SelectMany(v=>v.UVs).Any(uv=>uv!=Vector3.Zero),"Face UVs remain source placeholders");
                foreach(var t in v.Tangents)Check(Math.Abs(Vector3.Dot(new(t.X,t.Y,t.Z),v.Normal))<.00001f,"Face tangent is not orthogonal");
            }
            Check(result.Limitations.Any(l=>l.Contains("two-triangle"))&&result.Limitations.Any(l=>l.Contains("shader")),"Unverified auxiliary card/shader behavior is not reported");
            var assembled=NpcAppearance.Load(Read,defs,id);
            Check(assembled.Parts.Any(p=>p.Slot=="Face")&&assembled.Parts.Any(p=>p.Slot=="Hair"),"Absent helm did not assemble the real source face and hair");
            // All three generated meshes lead the composite. Their canonical
            // bind frames must leave the TRI world coordinates unchanged.
            for(int m=0;m<3;++m)for(int v=0;v<result.Model.Meshes[m].Vertices.Count;++v)
                Near(assembled.Model.Meshes[m].Vertices[v].Position,result.Model.Meshes[m].Vertices[v].Position,.000001f,"Canonical composition moved the generated head");
            var encoded=assembled.Model.Write();var readback=FLVER2.Read(encoded);geometryHashes.Add(Convert.ToHexString(SHA256.HashData(result.Model.Write())));
            DrawVariants(readback,"Canonical appearance readback");
            Check(readback.Meshes.Sum(m=>m.Vertices.Count)==assembled.Vertices,"Assembled source face serialization changed vertex count");
            for(int m=0;m<assembled.Model.Meshes.Count;m++)for(int v=0;v<assembled.Model.Meshes[m].Vertices.Count;v++) {
                var before=assembled.Model.Meshes[m].Vertices[v];var after=readback.Meshes[m].Vertices[v];
                for(int uv=0;uv<2;uv++) {var delta=Vector3.Abs(before.UVs[uv]-after.UVs[uv]);Check(delta.X<=.5001f/1024&&delta.Y<=.5001f/1024,"Composed source UV roundtrip scaled or wrapped a source set");}
                Check(before.Tangents.SequenceEqual(after.Tangents)&&before.Colors.Select(c=>(c.A,c.R,c.G,c.B)).SequenceEqual(after.Colors.Select(c=>(c.A,c.R,c.G,c.B))),"Packed UV layout changed source tangents or colors");
            }
            var targets=new List<object>();
            foreach(string target in new[]{"c8030","c2630"}) {
                var nativeFile=Path.Combine(native,"chr",target+".chrbnd.dcx");var nativeBytes=File.ReadAllBytes(nativeFile);hashes.TryAdd(nativeFile,SHA256.HashData(nativeBytes));
                var template=BND4.Read(nativeBytes).Files.Single(f=>f.Name.EndsWith(".flver"));var nativeModel=FLVER2.Read(template.Bytes);
                Check(NativeWinding(nativeModel,nativeModel.Meshes.Count)>.98f,"Original Bloodborne character culling convention changed");
                var baked=SkinRetarget.Bake(encoded,template.Bytes,assembled.Textures,null,null,"c6907");
                Check(baked.Vertices==assembled.Vertices&&baked.DirectWeightShare==1,"Native retarget lost source face geometry or direct weights");
                var outModel=FLVER2.Read(baked.Model.Write());var outTextures=TPF.Read(baked.Textures.Write());
                DrawVariants(outModel,"Native "+target+" FLVER readback");
                Check(NativeWinding(outModel,3)>.99f,"Native exported face is inside-out");
                for(int m=0;m<assembled.Model.Meshes.Count;m++)for(int v=0;v<assembled.Model.Meshes[m].Vertices.Count;v++)for(int uv=0;uv<2;uv++) {
                    var original=assembled.Model.Meshes[m].Vertices[v].UVs[uv];var sourceUv=readback.Meshes[m].Vertices[v].UVs[uv];var nativeUv=outModel.Meshes[m].Vertices[v].UVs[uv];
                    var sourceDelta=Vector3.Abs(nativeUv-sourceUv);var totalDelta=Vector3.Abs(nativeUv-original);
                    Check(sourceDelta.X<=.5001f/2048&&sourceDelta.Y<=.5001f/2048&&totalDelta.X<=.5001f/1024+.5001f/2048&&totalDelta.Y<=.5001f/1024+.5001f/2048,"Source-to-native UV serialization scaled/wrapped a source set");
                }
                var sourceWorld=SkinRetarget.World(assembled.Model.Nodes);var targetWorld=SkinRetarget.World(nativeModel.Nodes);int sh=assembled.Model.Nodes.FindIndex(n=>n.Name=="Head"),th=nativeModel.Nodes.FindIndex(n=>n.Name=="Head");
                Check(Matrix4x4.Invert(sourceWorld[sh],out var inverse),"Source Head inverse bind failed");var change=inverse*targetWorld[th];
                for(int m=0;m<3;++m)for(int v=0;v<assembled.Model.Meshes[m].Vertices.Count;++v) {
                    Near(outModel.Meshes[m].Vertices[v].Position,Vector3.Transform(assembled.Model.Meshes[m].Vertices[v].Position,change),.00003f,"Native Head bind places the face incorrectly");
                    Check(outModel.Meshes[m].Vertices[v].BoneIndices[0]==th,"Native face Head index changed");
                }
                Check(outTextures.Platform==TPF.TPFPlatform.PS4&&outTextures.Textures.Any(t=>t.Name.Contains($"FG_import_{id}_face")),"Native export lost the generated source face texture");
                targets.Add(new{target,baked.Vertices,baked.Triangles,baked.DirectWeightShare,textures=outTextures.Textures.Count});
            }
            reports.Add(new{id,result.HairStyle,assembled.Vertices,assembled.Triangles,seamVertices=generatedRim.Length,seamError,parts=assembled.Parts.Select(p=>p.Path),targets});
            Console.WriteLine($"PASS source FaceGen {id}: canonical face bounds, full EGT texture, source neck/hair, assembly and both native rig readbacks");
        }
        Check(geometryHashes.Count==2&&textureHashes.Count==2,"Distinct NPC face geometry/color collapsed");
        var selected=faces.Rows.Single(r=>r.ID==6270);var field=selected.Cells.Single(c=>c.Def.InternalName=="faceGeoData00");var old=field.Value;field.Value=(byte)(Convert.ToInt32(old)+1);
        var changed=NpcFace.Load(Read,selected,1);field.Value=old;Check(!geometryHashes.Contains(Convert.ToHexString(SHA256.HashData(changed.Model.Write()))),"Source face coefficients do not affect geometry");
        var assets=BND3.Read(Read("facegen/FaceGen.fgbnd")!);var egm=assets.Files.Single(f=>Leaf(f.Name)=="face.egm");var broken=egm.Bytes.ToArray();BitConverter.GetBytes(853).CopyTo(broken,8);egm.Bytes=broken;var bad=assets.Write();
        Reject(()=>NpcFace.Load(p=>p=="facegen/FaceGen.fgbnd"?bad:Read(p),selected,1));
        Reject(()=>NpcFace.Load(Read,selected,7));
        foreach(var pair in hashes)Check(SHA256.HashData(File.ReadAllBytes(pair.Key)).SequenceEqual(pair.Value),"Original source/native asset changed: "+pair.Key);
        string output="out/map-editor/firelink-validation/npc-face-tests.json";Directory.CreateDirectory(Path.GetDirectoryName(output)!);File.WriteAllText(output,JsonSerializer.Serialize(new{reports,originalAssetsChecked=hashes.Count},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS distinct faces, malformed basis rejection, original asset hashes ({hashes.Count}); no runtime shading claim");
    }
}

// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;using System.Numerics;using System.Security.Cryptography;using System.Buffers.Binary;using System.Text.Json;
internal static class BasinWaterTests {
    static void Check(bool value,string why){if(!value)throw new InvalidDataException(why);}
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Main(string[] args) {
        if(args.Length is <2 or >3)throw new ArgumentException("DS1R root, BB dvdroot required");
        var hashes=new Dictionary<string,string>();
        byte[] Read(string root,string path){string p=Path.Combine(root,path);var b=File.ReadAllBytes(p);var hash=Convert.ToHexString(SHA256.HashData(b));Check(!hashes.TryGetValue(p,out var prior)||prior==hash,"Input changed during reads");hashes.TryAdd(p,hash);return b;}
        var sourceMtd=BND3.Read(Read(args[0],"mtd/Mtd.mtdbnd.dcx"));var nativeMtd=BND4.Read(Read(args[1],"mtd/allmaterialbnd.mtdbnd.dcx"));
        MTD Definition(IReadOnlyList<BinderFile> files,string p)=>MTD.Read(files.Single(f=>Leaf(f.Name).Equals(Leaf(p),StringComparison.OrdinalIgnoreCase)).Bytes);
        var tint=new SourceMapMaterials(p=>Definition(sourceMtd.Files,p),p=>Definition(nativeMtd.Files,p),"m12_00_00_00");
        var ground=new FLVER2.Material("ground","M[ARSN].mtd",0);
        tint.Convert(new FLVER2.Material("source","A12_Ground[DSB][L].mtd",0),ground);
        Check(ground.MTD.Contains("BBImport_m12_material_"),"Basin tint definition was not private");
        Check(((float[])tint.Definitions.Single().Definition.Params.Single(p=>p.Name=="g_DiffuseMapColor").Value).All(v=>v==.5f),"Authored Basin ground half-gain lost");
        var water=new BasinWater(p=>Definition(sourceMtd.Files,p),p=>Read(args[1],p),p=>Definition(nativeMtd.Files,p));
        var lakeProfile=FLVER2.Read(Read(args[1],"map/m32_00_00_00/m32_00_00_00_005070.flver.dcx"));
        var lakeMaterial=lakeProfile.Materials.Single(m=>Leaf(m.MTD)=="M[ARSN]_water.mtd");
        var nativeGx=lakeProfile.GXLists[lakeMaterial.GXIndex].Single(g=>g.ID=="GX07");
        var expectedTextureNames=new[]{"m32_00_water_853_a","m32_00_water_854_a","m32_00_water_853_r","m32_00_water_854_r","m32_00_water_853_s","m32_00_water_854_s"};
        var textureRecords=new List<object>();
        Check(water.NativeTextures.Count==6,"Native water six-texture closure missing");
        using(var bank=new BXF4Reader(Read(args[1],"map/m32/m32_0003.tpfbhd"),Read(args[1],"map/m32/m32_0003.tpfbdt")))foreach(string originalName in expectedTextureNames) {
            var original=TPF.Read(bank.ReadFile(bank.Files.Single(f=>Leaf(f.Name)==originalName+".tpf.dcx")));var before=original.Write(DCX.Type.None);
            var image=water.NativeTextures.Values.Single(t=>t.Name=="BBImport_m12_water_native_"+originalName);
            // Mirror the actual map export's one-texture PS4 carrier. Parsing
            // and reversing only the owned name must recover every native
            // texture/header byte, including all mip levels and swizzle data.
            var owned=new TPF{Platform=TPF.TPFPlatform.PS4,Encoding=1,Flag2=3,Textures=[image]};var emitted=TPF.Read(owned.Write(DCX.Type.None));
            Check(emitted.Textures[0].Bytes.SequenceEqual(original.Textures[0].Bytes),"Native water mip/pixel payload was converted");
            Check(!ReferenceEquals(image.Bytes,original.Textures[0].Bytes)&&!ReferenceEquals(image.Header,original.Textures[0].Header),"Owned native water texture aliases source record");
            emitted.Textures[0].Name=originalName;
            Check(emitted.Write(DCX.Type.None).SequenceEqual(before),"Owned water texture changed fields other than its name");
            Check(original.Write(DCX.Type.None).SequenceEqual(before),"Original native texture record was mutated");
            Check(image.Bytes.Distinct().Take(2).Count()==2&&image.Header.Width>1&&image.Header.Height>1&&image.Mipmaps>1,"Native water dependency was replaced by a constant fallback");
            textureRecords.Add(new{source=originalName,owned=image.Name,image.Format,image.Mipmaps,image.Flags1,image.Type,header=image.Header,payloadLength=image.Bytes.Length,payloadSha256=Convert.ToHexString(SHA256.HashData(image.Bytes)),normalizedRecordSha256=Convert.ToHexString(SHA256.HashData(before))});
        }
        void NativeResponse(FLVER2.Material material,FLVER2 model) {
            var gx=model.GXLists[material.GXIndex].Single(g=>g.ID=="GX07");
            // Deliberately bounded conversion: only source normal scrolling
            // may change native GX07. Reflection/depth/refraction retain the
            // actual native lake contract, not guessed source equivalents.
            for(int index=0;index<29;index++)if(index is not(0 or 1 or 2 or 3 or 20 or 21))
                Check(gx.Data.AsSpan(index*4,4).SequenceEqual(nativeGx.Data.AsSpan(index*4,4)),"Unverified water depth/refraction/diffuse control changed");
            foreach(var slot in material.Textures.Where(t=>!t.ParamName.Contains("Bump",StringComparison.Ordinal))) {
                string name=Path.GetFileNameWithoutExtension(Leaf(slot.Path));Check(water.NativeTextures.ContainsKey(name),"Native water material references an unstaged texture");
                var expected=lakeMaterial.Textures.Single(t=>t.ParamName==slot.ParamName);Check(name.EndsWith(Path.GetFileNameWithoutExtension(Leaf(expected.Path)),StringComparison.Ordinal),"Native water texture routed to wrong sampler");
                Check(slot.TilingTypeU==expected.TilingTypeU&&slot.TilingTypeV==expected.TilingTypeV&&slot.Unk14==expected.Unk14&&slot.Unk18==expected.Unk18&&slot.Unk1C==expected.Unk1C,"Native water sampler metadata changed");
            }
        }
        var source=FLVER2.Read(Read(args[0],"map/m12_00_00_00/m1780B0A12.flver.dcx"));
        var map=MSB1.Read(Read(args[0],"map/MapStudio/m12_00_00_00.msb"));var part=map.Parts.MapPieces.Single(p=>p.ModelName=="m1780B0");
        var native=FLVER2.Read(Read(args[1],"map/m24_01_00_00/m24_01_00_00_004570.flver.dcx"));
        native.Meshes.Clear();native.Materials.Clear();native.Nodes.Clear();native.Nodes.Add(new FLVER.Node{Name="root",ParentIndex=-1,Scale=Vector3.One});native.Dummies.Clear();
        int donorLists=native.GXLists.Count;var oldLists=native.GXLists.Select(g=>g.Select(i=>Convert.ToHexString(i.Data)).ToArray()).ToArray();
        var geometry=new StaticMapGeometry(source,"DS1R");
        foreach(var mesh in source.Meshes) {
            var m=source.Materials[mesh.MaterialIndex];if(!water.IsSurface(m))continue;
            var (output,material)=water.Convert(m,mesh,geometry,native,"m40",(p,f)=>{Check(f=="normal"&&p!="","Native water dependency passed through source conversion/fallback");return "test_"+f;});
            foreach(var face in StaticMapGeometry.Faces(source,mesh))output.FaceSets.Add(new(FLVER2.FaceSet.FSFlags.None,false,face.Cull,face.Unknown,face.Indices));
            output.BoundingBox=new(){Min=output.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min),Max=output.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max)};
            native.Meshes.Add(output);native.Materials.Add(material);
            Check(output.Vertices.Count==mesh.Vertices.Count,"Lake geometry count changed");
            for(int i=0;i<mesh.Vertices.Count;i++) {
                Check(Vector3.Distance(output.Vertices[i].Position,geometry.Bake(mesh,mesh.Vertices[i]).Position)<.00001f,"Source rigid shoreline/height was lost");
                Check(output.Vertices[i].Tangents.Count==3&&output.Vertices[i].UVs.Count==3,"Native water three-layer vertex contract missing");
                for(int layer=0;layer<3;layer++) {
                    var slot=material.Textures.Single(t=>t.ParamName=="g_BumpmapTexture"+(layer==0?"":(layer+1).ToString()));
                    var scale=(float)Definition(sourceMtd.Files,m.MTD).Params.Single(p=>p.Name=="g_TileScale_"+layer).Value;
                    var actual=new Vector2(output.Vertices[i].UVs[layer].X,output.Vertices[i].UVs[layer].Y)*slot.TilingScale;
                    var expected=new Vector2(mesh.Vertices[i].UVs[0].X*scale,mesh.Vertices[i].UVs[0].Y*scale);
                    Check(Vector2.Distance(actual,expected)<.0001f,"Packed water UV compensation changed authored tiling");
                }
            }
            var gx=native.GXLists[material.GXIndex].Single(g=>g.ID=="GX07");float Float(int i)=>BinaryPrimitives.ReadSingleLittleEndian(gx.Data.AsSpan(i*4,4));
            Check(Math.Abs(Float(0)-.19f)<.00001f&&Math.Abs(Float(1)-.025f)<.00001f&&Math.Abs(Float(20)+.059f)<.00001f,"Source lake scroll lost");
            NativeResponse(material,native);
        }
        Check(water.Surfaces==1,"Authored lake surface missing");
        foreach(var definition in water.Definitions)Check(definition.Definition.Write(DCX.Type.None).SequenceEqual(Definition(nativeMtd.Files,lakeMaterial.MTD).Write(DCX.Type.None)),"Source absorption/specular values overwrote native lake material");
        for(int g=0;g<donorLists;g++)for(int i=0;i<oldLists[g].Length;i++)Check(Convert.ToHexString(native.GXLists[g][i].Data)==oldLists[g][i],"Shared native GX list changed");
        var roundtrip=FLVER2.Read(native.Write());
        Check(roundtrip.GXLists[roundtrip.Materials.Single().GXIndex].Single(i=>i.ID=="GX07").Data.SequenceEqual(native.GXLists[native.Materials.Single().GXIndex].Single(i=>i.ID=="GX07").Data),"Private water GX changed during serialization");
        var transform=Matrix4x4.CreateRotationY(part.Rotation.Y*MathF.PI/180)*Matrix4x4.CreateTranslation(part.Position+new Vector3(-143.8f,20.07f,164.63f));
        var points=roundtrip.Meshes.SelectMany(m=>m.Vertices).Select(v=>Vector3.Transform(v.Position,transform)).ToArray();
        Check(points.Min(p=>p.Y)>-51.71f&&points.Max(p=>p.Y)<-51.54f,"Basin water no longer follows original lake elevation");
        Check(roundtrip.Meshes.SelectMany(m=>m.Vertices).All(v=>v.UVs.Count==3&&v.Tangents.Count==3&&v.UVs.All(u=>Math.Abs(u.X)<=15&&Math.Abs(u.Y)<=15)),"Native packed water layout did not survive serialization");
        // Full installed-map census: every real reflective-water material,
        // including unplaced resource models, goes through the same adapter.
        var allWater=new BasinWater(p=>Definition(sourceMtd.Files,p),p=>Read(args[1],p),p=>Definition(nativeMtd.Files,p));
        var records=new List<object>();int sourceModels=0,reflectiveMeshes=0,totalVertices=0,sourceFadeVertices=0;float maxPackedError=0;
        foreach(string file in Directory.EnumerateFiles(Path.Combine(args[0],"map/m12_00_00_00"),"*.flver.dcx").Order(StringComparer.Ordinal)) {
            var model=FLVER2.Read(Read(args[0],Path.GetRelativePath(args[0],file)));sourceModels++;
            var selected=model.Meshes.Select((m,i)=>(Mesh:m,Index:i,Material:model.Materials[m.MaterialIndex])).Where(m=>allWater.IsSurface(m.Material)).ToArray();if(selected.Length==0)continue;
            var target=FLVER2.Read(Read(args[1],"map/m24_01_00_00/m24_01_00_00_004570.flver.dcx"));target.Meshes.Clear();target.Materials.Clear();target.Nodes.Clear();target.Nodes.Add(new(){Name="root",ParentIndex=-1,Scale=Vector3.One});target.Dummies.Clear();
            var geo=new StaticMapGeometry(model,"DS1R");var sources=new List<(FLVER2.Mesh Mesh,FLVER2.Material Material,FLVER2.Mesh Output,FLVER2.Material Result)>();
            foreach(var selectedMesh in selected) {
                var mesh=selectedMesh.Mesh;var material=selectedMesh.Material;var converted=allWater.Convert(material,mesh,geo,target,"m40",(path,fallback)=>{Check(fallback=="normal"&&path!="","Full-map native water textures went through source conversion");return "test_"+fallback;});
                foreach(var face in StaticMapGeometry.Faces(model,mesh))converted.Mesh.FaceSets.Add(new(FLVER2.FaceSet.FSFlags.None,false,face.Cull,face.Unknown,face.Indices));
                converted.Mesh.BoundingBox=new(){Min=converted.Mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min),Max=converted.Mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max)};
                target.Meshes.Add(converted.Mesh);target.Materials.Add(converted.Material);sources.Add((mesh,material,converted.Mesh,converted.Material));
            }
            var round=FLVER2.Read(target.Write());
            for(int index=0;index<sources.Count;index++) {
                var input=sources[index];var outputMesh=round.Meshes[index];var outputMaterial=round.Materials[index];var definition=Definition(sourceMtd.Files,input.Material.MTD);int alphaFade=input.Mesh.Vertices.Count(v=>v.Colors.Count>0&&v.Colors[0].A<1);sourceFadeVertices+=alphaFade;
                float factor=input.Output.Vertices.Select((v,i)=>input.Mesh.Vertices[i].UVs[0].X!=0?v.UVs[0].X/input.Mesh.Vertices[i].UVs[0].X:input.Mesh.Vertices[i].UVs[0].Y!=0?v.UVs[0].Y/input.Mesh.Vertices[i].UVs[0].Y:1).First();
                Check(outputMesh.Vertices.Count==input.Mesh.Vertices.Count&&outputMesh.FaceSets.Count==input.Mesh.FaceSets.Count(f=>f.Flags==FLVER2.FaceSet.FSFlags.None),"Water topology omitted during full-map conversion");
                for(int v=0;v<outputMesh.Vertices.Count;v++) {
                    var before=geo.Bake(input.Mesh,input.Mesh.Vertices[v],true);var after=outputMesh.Vertices[v];Check(Vector3.Distance(before.Position,after.Position)<.00001f,"Reflective source rigid geometry changed");Check(after.UVs.Count==3&&after.Tangents.Count==3&&after.UVs.All(u=>float.IsFinite(u.X)&&float.IsFinite(u.Y)&&Math.Abs(u.X)<=15&&Math.Abs(u.Y)<=15),"Packed layout clamped water UVs");
                    Check(after.Tangents.All(t=>Vector4.Distance(t,before.Tangents[0])<.02f)&&after.Tangents.Distinct().Count()==1,"Shared-UV water layers lost their authored tangent basis/handedness");
                    for(int layer=0;layer<3;layer++){var value=definition.Params.Single(p=>p.Name=="g_TileScale_"+layer).Value;var tile=value is float scalar?new Vector2(scalar):new Vector2(((float[])value)[0],((float[])value)[1]);var slot=outputMaterial.Textures.Single(t=>t.ParamName=="g_BumpmapTexture"+(layer==0?"":(layer+1).ToString()));var expected=new Vector2(input.Mesh.Vertices[v].UVs[0].X,input.Mesh.Vertices[v].UVs[0].Y)*tile;var actual=new Vector2(after.UVs[layer].X,after.UVs[layer].Y)*slot.TilingScale;float err=Vector2.Distance(expected,actual);maxPackedError=Math.Max(maxPackedError,err);Check(Math.Abs(actual.X-expected.X)<=Math.Abs(slot.TilingScale.X)/2048+.0001f&&Math.Abs(actual.Y-expected.Y)<=Math.Abs(slot.TilingScale.Y)/2048+.0001f,"Packed water UV phase exceeded one encoded step");}
                }
                var gx=round.GXLists[outputMaterial.GXIndex].Single(i=>i.ID=="GX07");Check(gx.Unk04==104&&gx.Data.Length==116,"Water GX ABI changed");
                NativeResponse(outputMaterial,round);
                foreach(var slot in outputMaterial.Textures.Where(t=>!t.ParamName.Contains("Bump",StringComparison.Ordinal)))Check(Vector2.Distance(slot.TilingScale*factor,lakeMaterial.Textures.Single(t=>t.ParamName==slot.ParamName).TilingScale)<.00001f,"Native water diffuse/specular/shininess frequency changed");
                for(int layer=0;layer<3;layer++){int at=layer==2?20:layer*2;var speed=(float[])definition.Params.Single(p=>p.Name=="g_TexScroll_"+layer).Value;bool moving=(int)definition.Params.Single(p=>p.Name=="g_TexScrollType").Value==1;Check(BinaryPrimitives.ReadSingleLittleEndian(gx.Data.AsSpan(at*4,4))==(moving?speed[0]:0)&&BinaryPrimitives.ReadSingleLittleEndian(gx.Data.AsSpan((at+1)*4,4))==(moving?speed[1]:0),"Full-map source normal scroll changed");}
                reflectiveMeshes++;totalVertices+=outputMesh.Vertices.Count;records.Add(new{file=Path.GetRelativePath(args[0],file),mesh=selected[index].Index,material=Leaf(input.Material.MTD),vertices=outputMesh.Vertices.Count,alphaFadeVertices=alphaFade,uvFactor=factor,sourceShoreFadeConverted=false});
            }
        }
        Directory.CreateDirectory("out/map-editor/basin-water/review-tests");File.WriteAllText("out/map-editor/basin-water/review-tests/census.json",JsonSerializer.Serialize(new{sourceModels,reflectiveMeshes,surfaces=allWater.Surfaces,definitions=allWater.Definitions.Select(d=>d.Name),records},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));Check(sourceModels==115&&reflectiveMeshes==16&&allWater.Surfaces==reflectiveMeshes&&allWater.Definitions.Count()==4,"Installed m12 reflective surface census changed");Check(sourceFadeVertices>0,"Source fade information was not audited");
        foreach(var definition in allWater.Definitions)Check(definition.Definition.Write(DCX.Type.None).SequenceEqual(Definition(nativeMtd.Files,lakeMaterial.MTD).Write(DCX.Type.None)),"Full-map private water MTD changed native response");
        var primary=source.Meshes.First(m=>water.IsSurface(source.Materials[m.MaterialIndex]));var badUv=new FLVER2.Mesh{MaterialIndex=primary.MaterialIndex,UseBoneWeights=false};badUv.Vertices.AddRange(primary.Vertices.Select(v=>new FLVER.Vertex(v)));badUv.Vertices[0].UVs[0]=new(float.PositiveInfinity,0,0);
        bool rejected=false;try{water.Convert(source.Materials[primary.MaterialIndex],badUv,geometry,native,"m40",(p,f)=>"invalid");}catch(InvalidDataException){rejected=true;}Check(rejected,"Infinite water UV was accepted");
        string report=args.Length==3?args[2]:"out/map-editor/basin-water/review-tests";Directory.CreateDirectory(report);File.WriteAllText(Path.Combine(report,"report.json"),JsonSerializer.Serialize(new{sourceModels,reflectiveMeshes,totalVertices,sourceFadeVertices,maxPackedError,textureRecords,records,limitations=BasinWater.Limitations,inputs=hashes},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        foreach(var (p,hash) in hashes)Check(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(p)))==hash,"Original archive changed");
        Console.WriteLine($"PASS: {reflectiveMeshes} reflective m12 meshes, six byte-exact owned native PS4 texture records, native MTD/depth/Fresnel response, primary lake geometry/elevation, three packed wave layers with shared authored tangent basis; source alpha omission audited; {hashes.Count} inputs unchanged. Reflections and colour require native visual QA.");
    }
}

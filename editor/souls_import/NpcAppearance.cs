// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Globalization;
using System.Numerics;

internal record NpcArmorPart(string Slot,int RequestedID,int BaseID,int ModelID,string Gender,string Path);
internal record NpcAppearanceResult(FLVER2 Model,List<TPF.Texture> Textures,List<NpcArmorPart> Parts,
    List<string> Limitations,int CharaInitID,int Sex,int FaceGenID,int Vertices,int Triangles) {
    public NpcBodyShape.Report? BodyShape {get;init;}
}

// DS1 human NPCs have a skeleton-only c0000 model. Their visible armor comes
// from CharaInit/Protector rows and separate parts binders, not that model.
internal static class NpcAppearance {
    const int VertexLimit=2000000;
    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static string TextureName(string name)=>Leaf(name).Split('.')[0];
    static List<BinderFile> Binder(byte[] bytes) {
        if(bytes.Length>512*1024*1024)throw new InvalidDataException("NPC source binder exceeds the import budget");
        if(DCX.Is(bytes))bytes=DCX.Decompress(bytes).ToArray();
        return BND3.Is(bytes)?BND3.Read(bytes).Files:BND4.Is(bytes)?BND4.Read(bytes).Files:throw new InvalidDataException("Unsupported NPC source binder");
    }
    static byte[] Required(Func<string,byte[]?> read,string path)=>read(path)??throw new FileNotFoundException("NPC appearance asset unavailable",path);
    static PARAM.Row Row(PARAM param,int id,string name) {
        var rows=param.Rows.Where(r=>r.ID==id).Take(2).ToArray();
        if(rows.Length!=1)throw new InvalidDataException($"Expected one {name} row {id}");
        return rows[0];
    }
    static int Cell(PARAM.Row row,string name) {
        var cell=row.Cells?.SingleOrDefault(c=>c.Def.InternalName==name)??throw new InvalidDataException("Missing NPC appearance field: "+name);
        return Convert.ToInt32(cell.Value,CultureInfo.InvariantCulture);
    }
    internal static NpcAppearanceResult Load(Func<string,byte[]?> read,string definitionsDirectory,int charaInitId,bool applyBodyShape=false) {
        var bytes=read("param/GameParam/GameParam.parambnd.dcx")??Required(read,"param/GameParam/GameParam.parambnd");
        var files=Binder(bytes);
        PARAM Param(string name) {
            var entry=files.SingleOrDefault(f=>Leaf(f.Name).Equals(name+".param",StringComparison.OrdinalIgnoreCase))??throw new InvalidDataException("Missing NPC appearance parameter: "+name);
            var param=PARAM.Read(entry.Bytes);param.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitionsDirectory,name+".xml")));return param;
        }
        return Assemble(read,Param("CharaInitParam"),Param("EquipParamProtector"),charaInitId,Param("FaceGenParam"),applyBodyShape);
    }
    internal static NpcAppearanceResult Assemble(Func<string,byte[]?> read,PARAM charaInit,PARAM protectors,int charaInitId,PARAM? faceGen=null,bool applyBodyShape=false) {
        var character=Row(charaInit,charaInitId,"CharaInitParam");
        int sex=Cell(character,"npcPlayerSex"),face=Cell(character,"npcPlayerFaceGenId");
        if(sex is not(0 or 1))throw new InvalidDataException("Unsupported DS1 NPC sex");
        var canonicalFiles=Binder(Required(read,"chr/c0000.chrbnd.dcx"));
        var canonical=ReadModel(canonicalFiles,"c0000");
        var parts=new List<NpcArmorPart>();var models=new List<FLVER2>();var textures=new Dictionary<string,TPF.Texture>(StringComparer.OrdinalIgnoreCase);var limitations=new List<string>();
        void AddTexture(TPF.Texture texture) {
            var name=TextureName(texture.Name);
            if(textures.TryGetValue(name,out var previous)) {
                if(previous.Format!=texture.Format||!previous.Bytes.AsSpan().SequenceEqual(texture.Bytes))throw new InvalidDataException("Conflicting NPC texture name: "+name);
            } else textures.Add(name,texture);
        }
        if(read("parts/Common_Body.tpf") is {} body)foreach(var texture in TPF.Read(body).Textures)AddTexture(texture);
        // The secondary body bank adds diffuse/normal resources referenced by
        // some armor subsets; keep the primary bank's higher-detail duplicate.
        if(read("parts/Common_Body_M.tpf") is {} bodyM)foreach(var texture in TPF.Read(bodyM).Textures)
            if(!textures.ContainsKey(TextureName(texture.Name)))AddTexture(texture);
        foreach(var slot in new[]{("Head","equip_Helm","HD",0,5),("Body","equip_Armer","BD",1,2),("Arms","equip_Gaunt","AM",2,1),("Legs","equip_Leg","LG",3,6)}) {
            int requested=Cell(character,slot.Item2);
            if(requested<0) {
                if(slot.Item1=="Head"&&faceGen!=null) {
                    var appearance=NpcFace.Load(read,Row(faceGen,face,"FaceGenParam"),sex);models.Add(appearance.Model);models.AddRange(appearance.SourceParts);
                    foreach(var texture in appearance.Textures)AddTexture(texture);limitations.AddRange(appearance.Limitations);
                    parts.Add(new("Face",requested,face,0,"A","facegen/FaceGen.fgbnd"));
                    parts.Add(new("Neck",requested,0,0,sex==1?"M":"F",appearance.SourcePaths.Single(p=>p.Contains("/FC_",StringComparison.Ordinal))));
                    parts.Add(new("Hair",requested,appearance.HairStyle,NpcFace.HairModel(appearance.HairStyle,sex),sex==1?"M":"F",appearance.SourcePaths.Single(p=>p.Contains("/HR_",StringComparison.Ordinal))));
                } else limitations.Add(slot.Item1=="Head"?$"Unarmored face/head and hair are not assembled; source FaceGen row {face} is not applied.":$"Unequipped {slot.Item1.ToLowerInvariant()} default body part is not assembled.");
                continue;
            }
            // CharaInit stores upgrade levels in the low three decimal digits.
            // Preserve exact rows when present; otherwise resolve their base armor.
            int baseId=protectors.Rows.Any(r=>r.ID==requested)?requested:requested/1000*1000;
            var armor=Row(protectors,baseId,"EquipParamProtector");
            if(Cell(armor,"protectorCategory")!=slot.Item4||Cell(armor,"equipModelCategory")!=slot.Item5)throw new InvalidDataException("NPC armor is assigned to an incompatible slot: "+slot.Item1);
            int model=Cell(armor,"equipModelId");if(model<0||model>9999)throw new InvalidDataException("Invalid NPC armor model number");
            string gender=Cell(armor,"equipModelGender") switch {0=>"A",1=>"M",2=>"F",3=>sex==1?"M":"F",4=>"M",5=>"E",_=>throw new InvalidDataException("Unsupported NPC armor model gender")};
            string path=$"parts/{slot.Item3}_{gender}_{model:D4}.partsbnd.dcx";
            var files=Binder(Required(read,path));var source=ReadModel(files,path);
            if(source.Meshes.Count==0)throw new InvalidDataException("NPC armor part has no visible meshes: "+path);
            foreach(var file in files)if(TPF.Is(file.Bytes.ToArray()))foreach(var texture in TPF.Read(file.Bytes).Textures)AddTexture(texture);
            parts.Add(new(slot.Item1,requested,baseId,model,gender,path));models.Add(source);
        }
        if(parts.Count==0)throw new InvalidDataException("NPC appearance has no supported equipped armor");
        // Cloth and weapon attachments require their own source adapters.
        // Appearance conversion does not establish source AI or quest behavior.
        if(parts.Any(p=>p.Slot=="Head"))limitations.Add($"Source FaceGen row {face}, hair and facial detail are not applied; this first assembly targets fully covered helmets.");
        limitations.Add(applyBodyShape?"Source cloth simulation, weapon models and weapon attachments are not assembled.":"Source body-shape customization, cloth simulation, weapon models and weapon attachments are not assembled.");
        limitations.Add("Appearance only: source animations, AI, dialogue and quest conditions are not converted. Runtime behavior must come from an explicit Bloodborne template.");
        var result=Compose(canonical,models);
        NpcBodyShape.Report? bodyShape=null;
        if(applyBodyShape) {
            var entries=canonicalFiles.Where(f=>Leaf(f.Name).Equals("c0000.bsipwv",StringComparison.OrdinalIgnoreCase)).ToArray();
            if(entries.Length!=1)throw new InvalidDataException("Source body-scale metadata is missing or ambiguous");
            int[] controls=new[]{"bodyScaleHead","bodyScaleBreast","bodyScaleAbdomen","bodyScaleArm","bodyScaleLeg"}.Select(n=>Cell(character,n)).ToArray();
            bodyShape=NpcBodyShape.Apply(result,entries[0].Bytes.ToArray(),controls);
            limitations=limitations.Select(l=>l.Replace("Face/eyes use the verified canonical Head bind and do not reproduce source eye animation or body-shape customization.","Face/eyes use the verified canonical Head bind; source eye animation is not reproduced.",StringComparison.Ordinal)).ToList();
            limitations.Add(bodyShape.Limitation);
        }
        foreach(var material in result.Materials) {
            var diffuse=material.Textures.FirstOrDefault(t=>t.ParamName is "g_Diffuse" or "g_DiffuseTexture");
            if(diffuse==null||!textures.ContainsKey(TextureName(diffuse.Path)))throw new InvalidDataException("NPC diffuse texture is unavailable: "+(diffuse?.Path??material.Name));
        }
        return new(result,textures.Values.ToList(),parts,limitations,charaInitId,sex,face,result.Meshes.Sum(m=>m.Vertices.Count),result.Meshes.SelectMany(m=>m.FaceSets).Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None).Sum(f=>f.Indices.Count/3)){BodyShape=bodyShape};
    }
    static FLVER2 ReadModel(List<BinderFile> files,string name) {
        var entry=files.SingleOrDefault(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).EndsWith(".flver.dcx",StringComparison.OrdinalIgnoreCase))??throw new InvalidDataException("Expected one NPC FLVER model: "+name);
        return FLVER2.Read(entry.Bytes);
    }
    internal static FLVER2 Compose(FLVER2 canonical,IReadOnlyList<FLVER2> parts) {
        if(canonical.Nodes.Count is <1 or >256)throw new InvalidDataException("NPC canonical skeleton exceeds the byte bone-index layout");
        var names=new Dictionary<string,int>(StringComparer.Ordinal);
        for(int i=0;i<canonical.Nodes.Count;i++)if(!names.TryAdd(canonical.Nodes[i].Name,i))throw new InvalidDataException("Ambiguous canonical NPC bone name: "+canonical.Nodes[i].Name);
        var targetWorld=SkinRetarget.World(canonical.Nodes);
        var output=new FLVER2();output.Header.Version=0x2000C;output.Header.Unicode=true;
        output.Nodes.AddRange(canonical.Nodes.Select(n=>new FLVER.Node(n){Flags=n.Flags}));
        // One explicit source layout avoids depending on each equipment file's
        // private vertex palette/layout. SkinRetarget later supplies native layouts.
        output.BufferLayouts.Add(new FLVER2.BufferLayout {
            new(FLVER.LayoutType.Float3,FLVER.LayoutSemantic.Position),
            new(FLVER.LayoutType.UByte4,FLVER.LayoutSemantic.BoneIndices),
            new(FLVER.LayoutType.Short4Norm,FLVER.LayoutSemantic.BoneWeights),
            new(FLVER.LayoutType.Float3,FLVER.LayoutSemantic.Normal),
            new(FLVER.LayoutType.Float4,FLVER.LayoutSemantic.Tangent,0),
            new(FLVER.LayoutType.Float4,FLVER.LayoutSemantic.Tangent,1),
            // Installed SoulsFormats scales Float2 UVs on write but not read.
            // One packed pair correctly round-trips both UV sets at 1/1024.
            new(FLVER.LayoutType.Short4,FLVER.LayoutSemantic.UV,0),
            new(FLVER.LayoutType.UByte4Norm,FLVER.LayoutSemantic.VertexColor)
        });
        int total=0;
        foreach(var part in parts) {
            var sourceWorld=SkinRetarget.World(part.Nodes);var deltas=new Dictionary<int,(int Bone,Matrix4x4 Position,Matrix4x4 Normal)>();
            (int Bone,Matrix4x4 Position,Matrix4x4 Normal) Delta(int sourceBone) {
                if(sourceBone<0||sourceBone>=part.Nodes.Count)throw new InvalidDataException("Invalid NPC equipment bone");
                if(deltas.TryGetValue(sourceBone,out var result))return result;
                if(!names.TryGetValue(part.Nodes[sourceBone].Name,out var target))throw new InvalidDataException("NPC equipment uses an unknown canonical bone: "+part.Nodes[sourceBone].Name);
                if(!Matrix4x4.Invert(sourceWorld[sourceBone],out var inverse))throw new InvalidDataException("Singular NPC equipment bind pose");
                var change=inverse*targetWorld[target];if(!Matrix4x4.Invert(change,out var normalInverse))throw new InvalidDataException("Singular NPC canonical bind pose");
                return deltas[sourceBone]=(target,change,Matrix4x4.Transpose(normalInverse));
            }
            foreach(var mesh in part.Meshes) {
                if(mesh.Dynamic is not(0 or 1)||mesh.MaterialIndex<0||mesh.MaterialIndex>=part.Materials.Count)throw new InvalidDataException("Unsupported NPC equipment mesh");
                if((long)total+mesh.Vertices.Count>VertexLimit)throw new InvalidDataException("NPC appearance exceeds the vertex budget");
                var material=part.Materials[mesh.MaterialIndex];
                var copiedMaterial=new FLVER2.Material(material.Name,material.MTD,material.Index){Textures=material.Textures.Select(t=>new FLVER2.Texture(t.ParamName,t.Path,t.TilingScale,t.TilingTypeU,t.TilingTypeV,t.Unk14,t.Unk18,t.Unk1C)).ToList()};
                var copied=new FLVER2.Mesh{UseBoneWeights=true,NodeIndex=-1,MaterialIndex=output.Materials.Count};copied.VertexBuffers.Add(new FLVER2.VertexBuffer(0));
                foreach(var vertex in mesh.Vertices) {
                    var v=new FLVER.Vertex(vertex);var position=Vector3.Zero;var normal=Vector3.Zero;var tangents=new Vector3[vertex.Tangents.Count];var weights=new Dictionary<int,float>();float sum=0;
                    for(int n=0;n<(mesh.Dynamic==1?4:1);n++) {
                        float weight=mesh.Dynamic==1?vertex.BoneWeights[n]:1;if(!float.IsFinite(weight)||weight<0)throw new InvalidDataException("Invalid NPC skin weight");if(weight==0)continue;
                        int sourceBone=SkinRetarget.Bone(mesh,vertex.BoneIndices[mesh.Dynamic==1?n:0]);var delta=Delta(sourceBone);
                        var point=vertex.Position;var normalMatrix=delta.Normal;var tangentMatrix=delta.Position;
                        if(mesh.Dynamic==0) {
                            point=Vector3.Transform(point,sourceWorld[sourceBone]);
                            if(!Matrix4x4.Invert(sourceWorld[sourceBone],out var inverse))throw new InvalidDataException("Singular rigid NPC equipment");
                            normalMatrix=Matrix4x4.Transpose(inverse)*normalMatrix;tangentMatrix=sourceWorld[sourceBone]*tangentMatrix;
                        }
                        position+=Vector3.Transform(point,delta.Position)*weight;normal+=Vector3.TransformNormal(vertex.Normal,normalMatrix)*weight;
                        for(int t=0;t<tangents.Length;t++)tangents[t]+=Vector3.TransformNormal(new(vertex.Tangents[t].X,vertex.Tangents[t].Y,vertex.Tangents[t].Z),tangentMatrix)*weight;
                        weights[delta.Bone]=weights.GetValueOrDefault(delta.Bone)+weight;sum+=weight;
                    }
                    if(sum<.0001f||weights.Count>4||!float.IsFinite(position.LengthSquared())||!float.IsFinite(normal.LengthSquared())||normal.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid assembled NPC vertex");
                    v.Position=position/sum;v.Normal=Vector3.Normalize(normal);v.NormalW=0;v.BoneIndices=new();v.BoneWeights=new();int wi=0;
                    foreach(var pair in weights.OrderByDescending(w=>w.Value)){v.BoneIndices[wi]=pair.Key;v.BoneWeights[wi++]=pair.Value/sum;}
                    v.Tangents.Clear();
                    for(int t=0;t<2;t++) {
                        var direction=t<tangents.Length?tangents[t]:Vector3.Zero;direction-=v.Normal*Vector3.Dot(v.Normal,direction);
                        if(direction.LengthSquared()<1e-12f)direction=Vector3.Cross(v.Normal,Math.Abs(v.Normal.X)<.9f?Vector3.UnitX:Vector3.UnitY);
                        if(!float.IsFinite(direction.LengthSquared()))throw new InvalidDataException("Invalid assembled NPC tangent");
                        v.Tangents.Add(new(Vector3.Normalize(direction),t<vertex.Tangents.Count?vertex.Tangents[t].W:1));
                    }
                    while(v.UVs.Count<2)v.UVs.Add(Vector3.Zero);if(v.UVs.Any(uv=>!float.IsFinite(uv.LengthSquared())))throw new InvalidDataException("Invalid assembled NPC UV");
                    if(v.UVs.Take(2).Any(uv=>MathF.Round(uv.X*1024)<short.MinValue||MathF.Round(uv.X*1024)>short.MaxValue||MathF.Round(uv.Y*1024)<short.MinValue||MathF.Round(uv.Y*1024)>short.MaxValue))
                        throw new InvalidDataException("Assembled NPC UV exceeds the packed source layout");
                    while(v.Colors.Count<1)v.Colors.Add(new FLVER.VertexColor(1f,1f,1f,1f));copied.Vertices.Add(v);
                }
                foreach(var face in mesh.FaceSets) {
                    if((face.Flags&FLVER2.FaceSet.FSFlags.EdgeCompressed)!=0)throw new InvalidDataException("Edge-compressed NPC equipment is unsupported");
                    var indices=face.Triangulate(mesh.Vertices.Count<65535);if(indices.Count%3!=0||indices.Any(i=>i<0||i>=copied.Vertices.Count))throw new InvalidDataException("Invalid NPC equipment triangles");
                    copied.FaceSets.Add(new(face.Flags,false,face.CullBackfaces,face.Unk06,indices));
                }
                if(copied.Vertices.Count==0||copied.FaceSets.Count==0)continue;
                copied.BoundingBox=new(){Min=copied.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min),Max=copied.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max)};
                output.Materials.Add(copiedMaterial);output.Meshes.Add(copied);total+=copied.Vertices.Count;
            }
        }
        if(output.Meshes.Count==0)throw new InvalidDataException("NPC appearance contains no geometry");
        output.Header.BoundingBoxMin=output.Meshes.Select(m=>m.BoundingBox.Min).Aggregate(Vector3.Min);output.Header.BoundingBoxMax=output.Meshes.Select(m=>m.BoundingBox.Max).Aggregate(Vector3.Max);
        return output;
    }
}

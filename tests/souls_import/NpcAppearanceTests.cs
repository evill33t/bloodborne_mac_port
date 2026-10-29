// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class NpcAppearanceTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Near(Vector3 actual,Vector3 expected)=>Check(Vector3.Distance(actual,expected)<.0001f,$"NPC vertex mismatch: {actual} != {expected}");
    static void Reject<T>(Action action) where T:Exception {
        try{action();}catch(T){return;}throw new Exception("Malformed NPC appearance accepted: "+typeof(T).Name);
    }
    static FLVER.Node Node(string name,Vector3 translation,short parent=-1)=>new(){Name=name,ParentIndex=parent,Translation=translation,Scale=Vector3.One};
    static FLVER2 Canonical() {
        var f=new FLVER2();f.Header.Version=0x2000C;
        f.Nodes=[Node("Root",Vector3.Zero),Node("Head",new(0,2,0),0),Node("Body",new(0,1,0),0)];return f;
    }
    static FLVER2 Part() {
        var f=new FLVER2();f.Header.Version=0x2000C;
        // A component has its own node order, palette and bind translations.
        f.Nodes=[Node("Body",new(0,3,0)),Node("Head",new(4,0,0)),Node("UnreferencedHelper",Vector3.Zero)];
        f.Materials=[new("#33# armor","C[ARSN].mtd",0){Textures=[new(){ParamName="g_Diffuse",Path="source.tif"}]}];
        f.BufferLayouts=[new(){new(FLVER.LayoutType.Float3,FLVER.LayoutSemantic.Position),new(FLVER.LayoutType.UByte4,FLVER.LayoutSemantic.BoneIndices),new(FLVER.LayoutType.Short4Norm,FLVER.LayoutSemantic.BoneWeights),new(FLVER.LayoutType.Float3,FLVER.LayoutSemantic.Normal),new(FLVER.LayoutType.Float4,FLVER.LayoutSemantic.Tangent),new(FLVER.LayoutType.Short4,FLVER.LayoutSemantic.UV),new(FLVER.LayoutType.UByte4Norm,FLVER.LayoutSemantic.VertexColor)}];
        var mesh=new FLVER2.Mesh{UseBoneWeights=true,MaterialIndex=0,BoneIndices=[1,0],VertexBuffers=[new(0)]};
        foreach(var point in new[]{new Vector3(5,0,0),new Vector3(0,4,0),new Vector3(4,3,0)})mesh.Vertices.Add(new(){Position=point,Normal=Vector3.UnitZ,Tangents=[new(1,0,0,-1)],UVs=[new(.25f,.75f,0),new(.375f,.125f,0)],Colors=[new(.5f,.25f,.75f,1f)]});
        mesh.Vertices[0].BoneIndices[0]=0;mesh.Vertices[0].BoneWeights[0]=1;
        mesh.Vertices[1].BoneIndices[0]=1;mesh.Vertices[1].BoneWeights[0]=1;
        mesh.Vertices[2].BoneIndices[0]=0;mesh.Vertices[2].BoneWeights[0]=.5f;mesh.Vertices[2].BoneIndices[1]=1;mesh.Vertices[2].BoneWeights[1]=.5f;
        mesh.FaceSets=[new(FLVER2.FaceSet.FSFlags.None,false,true,0,[0,1,2])];f.Meshes.Add(mesh);return f;
    }
    static PARAM Parameters(string type,IEnumerable<(int ID,Dictionary<string,int> Values)> rows) {
        var values=rows.ToArray();var def=new PARAMDEF{ParamType=type};
        foreach(var name in values.SelectMany(r=>r.Values.Keys).Distinct())def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.s32,name){Default=0});
        var result=new PARAM{ParamType=type,Rows=[]};
        foreach(var item in values){var row=new PARAM.Row(item.ID,"fixture",def);foreach(var field in item.Values)row.Cells.Single(c=>c.Def.InternalName==field.Key).Value=field.Value;result.Rows.Add(row);}return result;
    }
    static byte[] Binder(string name,byte[] bytes,TPF.Texture? texture=null) {
        var b=new BND3();b.Files.Add(new BinderFile(SoulsFormats.Binder.FileFlags.Flag1,0,name,bytes));
        if(texture!=null)b.Files.Add(new BinderFile(SoulsFormats.Binder.FileFlags.Flag1,1,"texture.tpf",new TPF{Textures=[texture]}.Write()));return b.Write();
    }
    static (PARAM Chara,PARAM Armor,Dictionary<string,byte[]> Assets) Fixture(int sex=1) {
        // Distinct exact CharaInit rows must survive; only missing armor upgrade
        // rows normalize to the base thousand. The shared texture is not in parts.
        var chara=Parameters("CHARA_INIT_PARAM",new[]{
            (6300,new Dictionary<string,int>{{"npcPlayerSex",sex},{"npcPlayerFaceGenId",6300},{"equip_Helm",100004},{"equip_Armer",101004},{"equip_Gaunt",102004},{"equip_Leg",103004}}),
            (6302,new Dictionary<string,int>{{"npcPlayerSex",sex},{"npcPlayerFaceGenId",6302},{"equip_Helm",100004},{"equip_Armer",101004},{"equip_Gaunt",102004},{"equip_Leg",103004}})});
        var armor=Parameters("EQUIP_PARAM_PROTECTOR_ST",new[]{(100000,0,5,0),(101000,1,2,3),(102000,2,1,0),(103000,3,6,3)}.Select(row=>(row.Item1,new Dictionary<string,int>{{"protectorCategory",row.Item2},{"equipModelCategory",row.Item3},{"equipModelGender",row.Item4},{"equipModelId",9220}})));
        var texture=new TPF.Texture("source",9,0,SkinRetarget.RgbaTexture([71,123,211,255]),TPF.TPFPlatform.PC);
        var assets=new Dictionary<string,byte[]>{{"chr/c0000.chrbnd.dcx",Binder("c0000.flver",Canonical().Write())},{"parts/Common_Body.tpf",new TPF{Textures=[texture]}.Write()}};
        foreach(var name in new[]{"HD_A_9220","BD_M_9220","BD_F_9220","AM_A_9220","LG_M_9220","LG_F_9220"})assets["parts/"+name+".partsbnd.dcx"]=Binder(name+".flver",Part().Write());
        return(chara,armor,assets);
    }
    internal static void Run() {
        var canonical=Canonical();var source=Part();var sourceBytes=source.Write();var canonicalBytes=canonical.Write();
        var composed=NpcAppearance.Compose(canonical,[source,source]);
        Check(composed.Meshes.Count==2&&composed.Materials.Count==2,"NPC component meshes were lost");
        Check(composed.Meshes[0].BoneIndices.Count==0&&composed.Meshes[0].UseBoneWeights,"NPC did not normalize to global canonical weights");
        var vertices=composed.Meshes[0].Vertices;Near(vertices[0].Position,new(1,2,0));Near(vertices[1].Position,new(0,2,0));Near(vertices[2].Position,new(2,3,0));
        Check(vertices[0].BoneIndices[0]==1&&vertices[1].BoneIndices[0]==2,"Component palette was not remapped by canonical node name");
        Check(Math.Abs(vertices[2].BoneWeights[0]-.5f)<.0001f&&Math.Abs(vertices[2].BoneWeights[1]-.5f)<.0001f,"Blended skin weights changed");
        Check(vertices[0].Tangents.Count==2&&vertices[0].UVs.Count==2&&vertices[0].Tangents[0].W==-1,"Source tangent/UV padding changed handedness");
        Near(vertices[0].UVs[0],new(.25f,.75f,0));Check(Math.Abs(vertices[0].Colors[0].R-.25f)<.0001f,"Source vertex color was replaced");
        var reopened=FLVER2.Read(composed.Write());Check(reopened.Meshes.Count==2&&reopened.Nodes.Count==3,"Composed NPC FLVER does not round trip");Near(reopened.Meshes[0].Vertices[2].Position,new(2,3,0));
        for(int m=0;m<composed.Meshes.Count;m++)for(int v=0;v<composed.Meshes[m].Vertices.Count;v++)for(int uv=0;uv<2;uv++)
            Near(reopened.Meshes[m].Vertices[v].UVs[uv],composed.Meshes[m].Vertices[v].UVs[uv]);
        Check(reopened.Meshes[0].Vertices[0].UVs[0]!=reopened.Meshes[0].Vertices[0].UVs[1],"Distinct source UV sets were merged");
        Check(sourceBytes.SequenceEqual(source.Write())&&canonicalBytes.SequenceEqual(canonical.Write()),"NPC assembly mutated a source model");
        // Nonuniform scale and rotation must transform normals independently from
        // positions and keep tangent directions perpendicular to the result.
        canonical.Nodes[1].Scale=new(2,1,1);canonical.Nodes[1].Rotation=new(0,0,.4f);source=Part();source.Meshes[0].Vertices[0].Normal=Vector3.Normalize(new(1,1,0));
        source.Meshes[0].Vertices[0].Tangents=[new(1,-1,0,1)];
        var scaled=NpcAppearance.Compose(canonical,[source]).Meshes[0].Vertices[0];
        var expected=Vector3.Transform(new Vector3(1,0,0),Matrix4x4.CreateScale(2,1,1)*Matrix4x4.CreateRotationZ(.4f)*Matrix4x4.CreateTranslation(0,2,0));Near(scaled.Position,expected);
        var expectedNormal=Vector3.Normalize(Vector3.TransformNormal(new Vector3(.5f,1,0),Matrix4x4.CreateRotationZ(.4f)));Near(scaled.Normal,expectedNormal);
        Check(Math.Abs(Vector3.Dot(scaled.Normal,new(scaled.Tangents[0].X,scaled.Tangents[0].Y,scaled.Tangents[0].Z)))<.0001f,"NPC tangent is not perpendicular to its transformed normal");
        source=Part();source.Meshes[0].UseBoneWeights=false;source.Meshes[0].Vertices[0].Position=Vector3.UnitX;
        Near(NpcAppearance.Compose(Canonical(),[source]).Meshes[0].Vertices[0].Position,new(1,2,0));
        source=Part();source.Meshes[0].Vertices[0].UVs[0]=new(32.1f,0,0);Reject<InvalidDataException>(()=>NpcAppearance.Compose(Canonical(),[source]));
        source=Part();source.Nodes[1].Name="UnknownWeightedBone";Reject<InvalidDataException>(()=>NpcAppearance.Compose(Canonical(),[source]));
        source=Part();source.Meshes[0].Vertices[0].BoneIndices[0]=10;Reject<InvalidDataException>(()=>NpcAppearance.Compose(Canonical(),[source]));
        source=Part();source.Meshes[0].Vertices[0].BoneWeights[0]=-.5f;Reject<InvalidDataException>(()=>NpcAppearance.Compose(Canonical(),[source]));
        source=Part();source.Meshes[0].FaceSets[0].Indices[2]=100;Reject<InvalidDataException>(()=>NpcAppearance.Compose(Canonical(),[source]));
        var fixture=Fixture();byte[]? Read(string name)=>fixture.Assets.GetValueOrDefault(name);
        var assembled=NpcAppearance.Assemble(Read,fixture.Chara,fixture.Armor,6302);
        Check(assembled.CharaInitID==6302&&assembled.FaceGenID==6302,"Exact NPC CharaInit variant was normalized away");
        Check(assembled.Parts.Count==4&&assembled.Vertices==12&&assembled.Triangles==4&&assembled.Textures.Count==1,"NPC appearance omitted armor or shared body textures");
        Check(assembled.Parts.Select(p=>p.BaseID).SequenceEqual(new[]{100000,101000,102000,103000}),"Upgraded NPC armor did not resolve to base rows");
        Check(assembled.Parts[0].Gender=="A"&&assembled.Parts[1].Gender=="M"&&assembled.Parts[3].Gender=="M","Male NPC armor gender resolved incorrectly");
        Check(assembled.Limitations.Any(s=>s.Contains("FaceGen"))&&assembled.Limitations.Any(s=>s.Contains("AI")),"Unsupported NPC face/behavior conversion was not reported");
        var female=Fixture(0);var femaleResult=NpcAppearance.Assemble(p=>female.Assets.GetValueOrDefault(p),female.Chara,female.Armor,6300);
        Check(femaleResult.Parts[1].Gender=="F"&&femaleResult.Parts[3].Gender=="F"&&femaleResult.Parts[0].Gender=="A","Female or unisex armor path resolved incorrectly");
        fixture.Assets.Remove("parts/Common_Body.tpf");Reject<InvalidDataException>(()=>NpcAppearance.Assemble(Read,fixture.Chara,fixture.Armor,6300));
        fixture=Fixture();fixture.Assets.Remove("parts/HD_A_9220.partsbnd.dcx");Reject<FileNotFoundException>(()=>NpcAppearance.Assemble(Read,fixture.Chara,fixture.Armor,6300));
        fixture=Fixture();fixture.Chara.Rows[0].Cells.Single(c=>c.Def.InternalName=="equip_Helm").Value=-1;
        var headless=NpcAppearance.Assemble(Read,fixture.Chara,fixture.Armor,6300);Check(headless.Parts.Count==3&&headless.Limitations.Any(s=>s.Contains("Unarmored face/head")),"Unequipped head was silently invented");
        fixture=Fixture();fixture.Armor.Rows[0].Cells.Single(c=>c.Def.InternalName=="protectorCategory").Value=3;Reject<InvalidDataException>(()=>NpcAppearance.Assemble(Read,fixture.Chara,fixture.Armor,6300));
        fixture=Fixture();fixture.Assets["parts/HD_A_9220.partsbnd.dcx"]=Binder("head.flver",Part().Write(),new("source",9,0,SkinRetarget.RgbaTexture([1,2,3,4]),TPF.TPFPlatform.PC));Reject<InvalidDataException>(()=>NpcAppearance.Assemble(Read,fixture.Chara,fixture.Armor,6300));
        Console.WriteLine("PASS: NPC armor IDs/genders/shared textures, canonical skeleton composition, palette/blend/rigid transforms, inverse-transpose normals, serialization, source preservation and explicit appearance limits");
    }
}

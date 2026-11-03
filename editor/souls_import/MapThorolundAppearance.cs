// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// Both the Maiden hood and Cleric helm leave the FaceGen face exposed. Keep
// that face and its neck; the equipped head part supplies the visible hood.
internal static class MapThorolundAppearance {
    internal sealed record Result(FLVER2 Model,List<TPF.Texture> Textures,IReadOnlyList<NpcArmorPart> Parts,NpcBodyShape.Report BodyShape,int FaceGenID,bool OpenHood);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Thorolund appearance: "+why);}
    internal static Result Load(Func<string,byte[]?> source,string definitions,int id) {
        Need(id is 6070 or 6080 or 6090 or 6100,"unowned source actor");
        byte[] Read(string path)=>source(path)??throw new FileNotFoundException("Thorolund appearance resource unavailable",path);
        var game=BND3.Read(Read("param/GameParam/GameParam.parambnd.dcx"));
        PARAM Param(string name){var p=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,name+".xml")));return p;}
        var chara=Param("CharaInitParam");var row=chara.Rows.Single(r=>r.ID==id);int N(string f)=>Convert.ToInt32(row[f].Value);
        int sex=id==6070?0:1;int[] equipment=id switch{6070=>[410000,411000,412000,413000],6080=>[-1,651000,652000,653000],6090=>[-1,181010,182010,183010],_=>[180010,181010,182010,183010]};
        Need(N("npcPlayerSex")==sex&&N("npcPlayerFaceGenId")==id&&new[]{"equip_Helm","equip_Armer","equip_Gaunt","equip_Leg"}.Select(N).SequenceEqual(equipment),"source CharaInit identity changed");
        if(equipment[0]<0){var a=NpcAppearance.Load(source,definitions,id,applyBodyShape:true);Need(a.BodyShape!=null&&a.Parts.Any(p=>p.Slot=="Face")&&a.Parts.Any(p=>p.Slot=="Hair"),"uncovered source face/hair missing");return new(a.Model,a.Textures,a.Parts,a.BodyShape!,id,false);}
        var protector=Param("EquipParamProtector");var helm=protector.Rows.Single(r=>r.ID==(id==6070?410000:180000));int modelID=id==6070?9521:9340;
        string faceSex=sex==0?"F":"M";
        Need(Convert.ToInt32(helm["equipModelId"].Value)==modelID&&Convert.ToInt32(helm["equipModelGender"].Value)==3&&new[]{"ScaleX","ScaleZ"}.All(f=>Convert.ToSingle(helm["faceScale"+faceSex+"_"+f].Value)==1),"source open hood face scale changed");
        var armor=NpcAppearance.Assemble(source,chara,protector,id,Param("FaceGenParam"));var face=NpcFace.Load(source,Param("FaceGenParam").Rows.Single(r=>r.ID==id),sex);
        Need(armor.Parts.Count==4&&armor.Parts.All(p=>p.ModelID==modelID)&&face.SourceParts.Count==(id==6070?2:1)&&face.HairStyle==(id==6070?8:0),"source armor/skin ownership changed");
        var skinPaths=face.SourcePaths.Where(p=>p.Contains("/FC_",StringComparison.Ordinal)||p.Contains("/HR_",StringComparison.Ordinal)).ToArray();Need(skinPaths.Length==2&&skinPaths[0].Contains("/FC_",StringComparison.Ordinal),"source neck ordering changed");
        var canonicalBank=BND3.Read(Read("chr/c0000.chrbnd.dcx"));var canonical=FLVER2.Read(canonicalBank.Files.Single(f=>Leaf(f.Name)=="c0000.flver").Bytes);
        var model=NpcAppearance.Compose(canonical,[armor.Model,face.Model,face.SourceParts[0]]);
        string TextureName(string p)=>Path.GetFileNameWithoutExtension(Leaf(p));
        var wanted=face.Model.Materials.Concat(face.SourceParts[0].Materials).SelectMany(m=>m.Textures).Select(t=>TextureName(t.Path)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var textures=new Dictionary<string,TPF.Texture>(StringComparer.OrdinalIgnoreCase);
        foreach(var texture in armor.Textures.Concat(face.Textures.Where(t=>wanted.Contains(TextureName(t.Name))))) {
            if(textures.TryGetValue(texture.Name,out var previous))Need(previous.Format==texture.Format&&previous.Bytes.AsSpan().SequenceEqual(texture.Bytes),"conflicting appearance texture "+texture.Name);else textures.Add(texture.Name,texture);
        }
        int[] controls=new[]{"bodyScaleHead","bodyScaleBreast","bodyScaleAbdomen","bodyScaleArm","bodyScaleLeg"}.Select(N).ToArray();
        Need(controls.SequenceEqual(id==6070?new[]{-30,50,-30,-50,30}:new[]{-50,100,0,100,50}),"source body controls changed");
        var shape=NpcBodyShape.Apply(model,canonicalBank.Files.Single(f=>Leaf(f.Name)=="c0000.bsipwv").Bytes.ToArray(),controls);
        var parts=armor.Parts.Concat(new[]{new NpcArmorPart("Face",-1,id,0,"A","facegen/FaceGen.fgbnd"),new NpcArmorPart("Neck",-1,0,0,faceSex,skinPaths[0])}).ToArray();
        return new(model,textures.Values.ToList(),parts,shape,id,true);
    }
}

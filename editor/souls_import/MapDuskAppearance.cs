// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// Dusk's crown is open faced. A helmet row is not evidence that the source
// parts binder contains her face; retain FaceGen6050 beneath the crown.
internal static class MapDuskAppearance {
    internal sealed record Result(FLVER2 Model,List<TPF.Texture> Textures,NpcAppearanceResult Armor,NpcFaceResult Face,NpcBodyShape.Report BodyShape);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Dusk appearance: "+why);}
    internal static Result Load(Func<string,byte[]?> source,string definitions) {
        byte[] Read(string path)=>source(path)??throw new FileNotFoundException("Dusk source appearance unavailable",path);
        var game=BND3.Read(Read("param/GameParam/GameParam.parambnd.dcx"));
        PARAM Param(string name){var p=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,name+".xml")));return p;}
        var chara=Param("CharaInitParam");var row=chara.Rows.Single(r=>r.ID==6050);
        int N(string field)=>Convert.ToInt32(row[field].Value);
        Need(N("npcPlayerSex")==0&&N("npcPlayerFaceGenId")==6050&&new[]{N("equip_Helm"),N("equip_Armer"),N("equip_Gaunt"),N("equip_Leg")}.SequenceEqual(new[]{330000,331000,332000,333000}),"source CharaInit6050 identity changed");
        var protector=Param("EquipParamProtector");var crown=protector.Rows.Single(r=>r.ID==330000);
        Need(Convert.ToInt32(crown["equipModelId"].Value)==9480&&Convert.ToInt32(crown["equipModelGender"].Value)==3,"source crown identity changed");
        var armor=NpcAppearance.Assemble(source,chara,protector,6050,Param("FaceGenParam"));
        Need(armor.Parts.Count==4&&armor.Parts.All(p=>p.ModelID==9480),"source Antiquated set changed");
        var face=NpcFace.Load(source,Param("FaceGenParam").Rows.Single(r=>r.ID==6050),0);
        var canonicalBank=BND3.Read(Read("chr/c0000.chrbnd.dcx"));var canonical=FLVER2.Read(canonicalBank.Files.Single(f=>Leaf(f.Name)=="c0000.flver").Bytes);
        // HD_F9480 supplies the authored hair/crown mesh (#27). Keep only the
        // FC neck from default skin parts; a second default hairstyle clips it.
        var neck=face.SourcePaths.Select((path,i)=>(path,i)).Where(p=>p.path.Contains("/FC_",StringComparison.Ordinal)).ToArray();
        Need(neck.Length==1&&face.SourceParts.Count==2,"source neck/hair ownership changed");
        var skinParts=face.SourcePaths.Where(p=>p.Contains("/FC_",StringComparison.Ordinal)||p.Contains("/HR_",StringComparison.Ordinal)).ToArray();
        Need(skinParts.Length==2&&skinParts[0].Contains("/FC_",StringComparison.Ordinal),"source face-part ordering changed");
        var model=NpcAppearance.Compose(canonical,[armor.Model,face.Model,face.SourceParts[0]]);
        var textures=new Dictionary<string,TPF.Texture>(StringComparer.OrdinalIgnoreCase);
        string TextureName(string path)=>Path.GetFileNameWithoutExtension(Leaf(path));
        var faceTextures=face.Model.Materials.Concat(face.SourceParts[0].Materials).SelectMany(m=>m.Textures).Select(t=>TextureName(t.Path)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach(var texture in armor.Textures.Concat(face.Textures.Where(t=>faceTextures.Contains(TextureName(t.Name))))) {
            if(textures.TryGetValue(texture.Name,out var previous))Need(previous.Format==texture.Format&&previous.Bytes.AsSpan().SequenceEqual(texture.Bytes),"conflicting appearance texture: "+texture.Name);
            else textures.Add(texture.Name,texture);
        }
        var bsipwv=canonicalBank.Files.Single(f=>Leaf(f.Name)=="c0000.bsipwv").Bytes.ToArray();
        int[] controls=new[]{"bodyScaleHead","bodyScaleBreast","bodyScaleAbdomen","bodyScaleArm","bodyScaleLeg"}.Select(N).ToArray();
        var shape=NpcBodyShape.Apply(model,bsipwv,controls);return new(model,textures.Values.ToList(),armor,face,shape);
    }
}

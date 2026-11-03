// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// The Black Sorcerer hat is open faced. Its armor binder does not replace
// Griggs's FaceGen head; retain the real face and neck beneath the source hat.
internal static class MapGriggsAppearance {
    internal sealed record Result(FLVER2 Model,List<TPF.Texture> Textures,NpcAppearanceResult Armor,NpcFaceResult Face,NpcBodyShape.Report BodyShape);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Griggs appearance: "+why);}
    internal static Result Load(Func<string,byte[]?> source,string definitions) {
        byte[] Read(string path)=>source(path)??throw new FileNotFoundException("Griggs appearance resource unavailable",path);
        var game=BND3.Read(Read("param/GameParam/GameParam.parambnd.dcx"));
        PARAM Param(string name){var p=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,name+".xml")));return p;}
        var chara=Param("CharaInitParam");var row=chara.Rows.Single(r=>r.ID==6040);int N(string field)=>Convert.ToInt32(row[field].Value);
        Need(N("npcPlayerSex")==1&&N("npcPlayerFaceGenId")==6040&&new[]{N("equip_Helm"),N("equip_Armer"),N("equip_Gaunt"),N("equip_Leg")}.SequenceEqual(new[]{640004,641004,642004,643004}),"source CharaInit6040 equipment/face changed");
        var protector=Param("EquipParamProtector");var hat=protector.Rows.Single(r=>r.ID==640000);
        Need(Convert.ToInt32(hat["equipModelId"].Value)==9372&&Convert.ToInt32(hat["equipModelGender"].Value)==3&&
            new[]{"faceScaleM_ScaleX","faceScaleM_ScaleZ","faceScaleM_MaxX","faceScaleM_MaxZ"}.All(f=>Convert.ToSingle(hat[f].Value)==1),"source open hat/face-scale changed");
        var armor=NpcAppearance.Assemble(source,chara,protector,6040,Param("FaceGenParam"));
        Need(armor.Parts.Count==4&&armor.Parts.Single(p=>p.Slot=="Head").Path=="parts/HD_M_9372.partsbnd.dcx","source Black Sorcerer set changed");
        var face=NpcFace.Load(source,Param("FaceGenParam").Rows.Single(r=>r.ID==6040),1);
        var skinParts=face.SourcePaths.Where(p=>p.Contains("/FC_",StringComparison.Ordinal)||p.Contains("/HR_",StringComparison.Ordinal)).ToArray();
        Need(face.SourceParts.Count==2&&skinParts.Length==2&&skinParts[0].Contains("/FC_",StringComparison.Ordinal),"source neck ordering changed");
        var canonicalBank=BND3.Read(Read("chr/c0000.chrbnd.dcx"));var canonical=FLVER2.Read(canonicalBank.Files.Single(f=>Leaf(f.Name)=="c0000.flver").Bytes);
        // The equipped hat owns the visible hairstyle. Adding the default hair
        // would make a second hairstyle clip the hat, as with Dusk's crown.
        var model=NpcAppearance.Compose(canonical,[armor.Model,face.Model,face.SourceParts[0]]);
        var wanted=face.Model.Materials.Concat(face.SourceParts[0].Materials).SelectMany(m=>m.Textures).Select(t=>Path.GetFileNameWithoutExtension(Leaf(t.Path))).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var textures=new Dictionary<string,TPF.Texture>(StringComparer.OrdinalIgnoreCase);
        foreach(var texture in armor.Textures.Concat(face.Textures.Where(t=>wanted.Contains(Path.GetFileNameWithoutExtension(Leaf(t.Name)))))) {
            if(textures.TryGetValue(texture.Name,out var previous))Need(previous.Format==texture.Format&&previous.Bytes.AsSpan().SequenceEqual(texture.Bytes),"conflicting source appearance texture "+texture.Name);
            else textures.Add(texture.Name,texture);
        }
        int[] controls=new[]{"bodyScaleHead","bodyScaleBreast","bodyScaleAbdomen","bodyScaleArm","bodyScaleLeg"}.Select(N).ToArray();
        Need(controls.SequenceEqual(new[]{-20,20,-10,-10,-10}),"source body customization changed");
        var shape=NpcBodyShape.Apply(model,canonicalBank.Files.Single(f=>Leaf(f.Name)=="c0000.bsipwv").Bytes.ToArray(),controls);
        return new(model,textures.Values.ToList(),armor,face,shape);
    }
}

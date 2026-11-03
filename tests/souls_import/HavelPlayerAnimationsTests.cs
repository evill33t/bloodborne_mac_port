// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
static class HavelPlayerAnimationsTests {
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    static void Main(string[] args) {
        var hashes=new Dictionary<string,string>();
        byte[] Read(string p){var b=File.ReadAllBytes(p);hashes[p]=Convert.ToHexStringLower(SHA256.HashData(b));return b;}
        byte[]? Source(string p)=>File.Exists(Path.Combine(args[0],p))?Read(Path.Combine(args[0],p)):null;byte[] Native(string p)=>Read(Path.Combine(args[1],p));
        string defs=Environment.GetEnvironmentVariable("SMITHBOX_DIR")+"/src/Smithbox.Data/Assets/PARAM/DS1R/Defs";
        var equipment=HavelEquipment.Build(Source,Native,defs);var skin=equipment.Skin;
        var player=FLVER2.Read(BND4.Read(Native("chr/c0000.chrbnd.dcx")).Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
        var playerWorld=SkinRetarget.World(player.Nodes);var playerNames=player.Nodes.Select((n,i)=>(n.Name,i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
        foreach(var (path,bytes) in equipment.Files.Where(p=>p.Key.StartsWith("parts/",StringComparison.Ordinal))) {
            var part=BND4.Read(bytes);var geometry=FLVER2.Read(part.Files.Single(f=>f.ID==200).Bytes);
            Check(geometry.Meshes.All(m=>m.NodeIndex>=0&&m.NodeIndex<geometry.Nodes.Count),"Player part lacks a real mesh node");
            bool weapon=path.Contains("/wp_",StringComparison.Ordinal);
            Check(geometry.Materials.All(m=>weapon?m.MTD.EndsWith("P[Weapon]_[ARSN]_NoCM.mtd",StringComparison.Ordinal):new[]{"P[ARSN].mtd","P[ARSN]_e.mtd","P[ARSN]_al.mtd"}.Contains(Leaf(m.MTD),StringComparer.Ordinal)),"Player part uses an enemy shader");
            Check(geometry.Meshes.All(m=>m.UseBoneWeights!=weapon),"Rigid weapon/skinned armor runtime contract changed");
            if(!weapon){
                int active=geometry.Nodes.TakeWhile(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0).Count();var world=SkinRetarget.World(geometry.Nodes);
                foreach(var m in geometry.Meshes)foreach(var v in m.Vertices)for(int i=0;i<4;i++)if(v.BoneWeights[i]>0) {
                    int at=SkinRetarget.Bone(m,v.BoneIndices[i]);Check(at<active,"Player armor weights an inactive palette entry");
                    Check(playerNames.TryGetValue(geometry.Nodes[at].Name,out int nativeAt)&&HavelEquipment.BindError(world[at],playerWorld[nativeAt])<.001f,"Player armor weights a placeholder bind: "+geometry.Nodes[at].Name);
                }
            }
            if(path.Contains("_l.partsbnd",StringComparison.Ordinal))Check(bytes.AsSpan().SequenceEqual(equipment.Files[path.Replace("_l.partsbnd",".partsbnd",StringComparison.Ordinal)]),"First-backread LOD differs from its owned part");
        }
        var parameters=HavelParameters.Build(Native("param/gameparam/gameparam.parambnd.dcx"),Source,defs);
        var ai=HavelAi.Build(Native("script/m24_00_00_00.luabnd.dcx"),Source,Native,4032799);
        var originalGame=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));var game=BND4.Read(parameters);
        var edited=new HashSet<string>{"ItemLotParam.param","CharaInitParam.param","NpcParam.param","NpcThinkParam.param","EquipParamProtector.param","EquipParamWeapon.param","WeaponGenParam.param","BehaviorParam_PC.param","AtkParam_Pc.param"};
        foreach(var entry in originalGame.Files) {
            var changed=game.Files.Single(f=>f.ID==entry.ID);Check(changed.Name==entry.Name,"Native parameter binder identity changed");
            if(!edited.Contains(Leaf(entry.Name))){Check(changed.Bytes.Span.SequenceEqual(entry.Bytes.Span),"Unowned native parameter file changed");continue;}
            PARAM Raw(ReadOnlyMemory<byte> bytes){var p=PARAM.Read(bytes.ToArray());return ImportParam.Read(bytes.ToArray(),p.ParamType,checked((int)p.DetectedSize),p.ParamdefDataVersion);}
            var before=Raw(entry.Bytes);var after=Raw(changed.Bytes);var ids=before.Rows.Select(r=>r.ID).ToHashSet();var retained=after.Rows.Where(r=>ids.Contains(r.ID)).ToArray();
            Check(retained.Length==before.Rows.Count&&before.Rows.Zip(retained).All(p=>p.First.ID==p.Second.ID&&p.First.Name==p.Second.Name&&ImportParam.Payload(p.First).AsSpan().SequenceEqual(ImportParam.Payload(p.Second))),"Original parameter rows/padding/order changed: "+entry.Name);
        }
        var npc=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);npc.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(defs+"/../../BB/Defs/NpcParam.xml")));
        Check(Convert.ToInt32(npc.Rows.Single(r=>r.ID==HavelParameters.Character)["itemLotId_1"].Value)==MapItems.VialLot,"Havel lacks the shared ten-percent Blood Vial roll");
        var gens=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)=="WeaponGenParam.param").Bytes);gens.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(defs+"/../../BB/Defs/WeaponGenParam.xml")));
        foreach(int id in new[]{HavelParameters.Tooth,HavelParameters.Shield})Check(Convert.ToInt32(gens.Rows.Single(r=>r.ID==id)["wepParamId"].Value)==id,"NPC generated equipment does not resolve its owned weapon");
        var info=LUAINFO.Read(BND4.Read(ai).Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo")).Bytes);
        var think=PARAM.Read(game.Files.Single(f=>Leaf(f.Name)=="NpcThinkParam.param").Bytes);think.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(defs+"/../../BB/Defs/NpcThinkParam.xml")));
        Check(Convert.ToInt32(think.Rows.Single(r=>r.ID==HavelParameters.Think)["logicId"].Value)==10000,"Havel entered native weapon-switch logic instead of source generic logic");
        Check(info.Goals.Count(g=>g.ID==HavelParameters.Think&&g.Name==HavelAi.Name&&g.BattleInterrupt)==1&&info.Goals.Any(g=>g.ID==10000&&g.LogicInterrupt),"Havel native logic/source battle registry missing");
        Console.WriteLine($"Havel armor direct weight share {skin.DirectWeightShare}; native nodes {skin.Model.Nodes.Count}; indirect: "+string.Join(",",skin.Bones.Where(b=>!b.Direct).Select(b=>b.Source+"->"+b.Target)));
        Check(skin.DirectWeightShare>.999999&&FLVER2.Read(skin.Model.Write()).Nodes.Count==310,"Havel armor weights/native player render rig changed");
        var result=HavelPlayerAnimations.Build(Source,Native,skin.Bones);
        var main=BND4.Read(result.Main);var split=BND4.Read(result.Split);var original=BND4.Read(Native("chr/c0000.anibnd.dcx"));
        var reachable=new HashSet<int>();
        foreach(var (p,b) in result.NativeSplits) {
            var archive=BND4.Read(b);foreach(var f in BND4.Read(Native(p)).Files)Check(archive.Files.Single(x=>x.ID==f.ID).Name==f.Name&&archive.Files.Single(x=>x.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Retail split member changed");
            foreach(var f in archive.Files.Where(f=>split.Files.Any(owned=>owned.ID==f.ID&&owned.ID>=1000000000)))Check(reachable.Add(f.ID),"Private motion has duplicate native resource owners");
        }
        Check(split.Files.Where(f=>f.ID>=1000000000).All(f=>reachable.Contains(f.ID)),"Private motion is outside the native14-path resource table");
        foreach(var f in original.Files.Where(f=>f.ID is not(7000400 or 7000500)))Check(main.Files.Single(x=>x.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Original native player file changed: "+f.Name);
        Console.WriteLine($"Private bindings: {result.Selectors} selectors, {result.Bindings.Count} source moves, {result.NativeCopies} native copies");
        Check(result.Selectors>0&&result.Bindings.Count>=50,"Havel private bindings missing");
        foreach(var binding in result.Bindings) {
            var tae=TAE.Read(main.Files.Single(f=>f.ID==5000000+binding.Bank).Bytes);var action=tae.Animations.Single(a=>a.ID==binding.Action);
            var file=split.Files.Single(f=>f.ID==1000000000+binding.Bank*1000000+binding.Action);var pack=new NativeAnimation.Pack(file.Bytes.ToArray());int animation=pack.Object("hkaSplineCompressedAnimation");
            Check(Math.Abs(pack.Float(animation+16)-binding.Duration)<.0001f&&pack.Int(animation+20)==108,"Converted clip duration/track count changed");
            Check(action.Events.Count(e=>e.Type==1)==binding.Contacts&&action.Events.All(e=>e.Group==null||action.EventGroups.Contains(e.Group)),"Contact/event group serialization failed");
            foreach(var hit in action.Events.Where(e=>e.Type==1)){var b=hit.GetParameterBytes(false);Check(b.Length==16&&BitConverter.ToInt32(b,12)==0,"Source player contact ABI mismatch");}
        }
        foreach(var (p,sha) in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(p)))==sha,"Original source/native asset changed");
        string output="out/map-editor/havel-hydra/animations-stage";Directory.CreateDirectory(output);File.WriteAllBytes(output+"/c0000.anibnd.dcx",result.Main);File.WriteAllBytes(output+"/c0000_ds1_havel.anibnd.dcx",result.Split);
        foreach(var (p,b) in equipment.Files){string path=output+"/"+p;Directory.CreateDirectory(Path.GetDirectoryName(path)!);File.WriteAllBytes(path,b);}
        foreach(var (p,b) in result.NativeSplits){string path=output+"/"+p;Directory.CreateDirectory(Path.GetDirectoryName(path)!);File.WriteAllBytes(path,b);}
        File.WriteAllBytes(output+"/gameparam.parambnd.dcx",parameters);File.WriteAllBytes(output+"/havel.luabnd.dcx",ai);
        Console.WriteLine($"PASS: Havel armor {skin.Vertices} vertices, {result.Bindings.Count} source player moves, {result.NativeCopies} private native fallback clips, {result.Selectors} selectors; {hashes.Count} original archives unchanged. Package fixture only; gameplay pending.");
    }
}

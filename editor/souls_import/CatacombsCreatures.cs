// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class CatacombsCreatures {
    internal sealed record Cohort(int Slot,int SourceCaster,int Caster,int DeadFlag,IReadOnlyList<int> Skeletons);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,IReadOnlyList<MapEnemySpawns.Spawn> Spawns,IReadOnlyList<Cohort> Cohorts,IReadOnlyList<MapEnemySpawns.Omitted> Skipped,IReadOnlyList<string> Limitations,IReadOnlyList<string> SharedNativeEffectVariants,IReadOnlyDictionary<string,CatacombsCreaturesPhysics.Report> Physics);
    static readonly (int Model,string Source,int Npc,int Offset)[] Families=[(8840,"c2900",290020,0),(8841,"c2900",290021,10000),(8842,"c2900",290022,20000),(8843,"c2910",291002,0),(8844,"c2650",265000,0),(8845,"c2930",293000,0)];
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs creatures: "+why);}
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]?> staged,Func<string,byte[]> native,
        MSB1 from,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,string definitions,string targetMap,byte[] scriptBytes) {
        Need(chunks.Count==slots.Count&&slots.Count>0,"collision binding contract");var plan=CatacombsCreaturesSource.Load(source,from,definitions);var files=new Dictionary<string,byte[]>();
        byte[] Read(string p)=>files.GetValueOrDefault(p)??staged(p)??native(p);
        var donorMap=MSBB.Read(native("map/MapStudio/m22_00_00_00.msb.dcx"));var donor=donorMap.Parts.Enemies.First(p=>p.ModelName=="c2610"&&p.NPCParamID==261010&&p.ThinkParamID==261010);
        var owners=new List<CatacombsCreaturesParameters.Owner>();var skins=new Dictionary<int,CatacombsCreaturesCharacters.Result>();var masks=new Dictionary<int,IReadOnlySet<int>>();
        foreach(var f in Families) {
            string model="c"+f.Model;Need(!target.Models.Enemies.Any(m=>m.Name==model)&&staged("chr/"+model+".chrbnd.dcx")==null,"owned model occupied: "+model);
            var npc=plan.Npcs.Rows.Single(r=>r.ID==f.Npc);var selected=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(npc["modelDispMask"+i].Value)!=0).ToHashSet();masks.Add(f.Model,selected);
            var character=CatacombsCreaturesCharacters.Build(source,native,f.Source,f.Offset,model,selected,files);skins.Add(f.Model,character);
            owners.Add(new(f.Model,f.Npc,plan.Ordinary.Where(p=>p.NPCParamID==f.Npc).Select(p=>p.ThinkParamID).Distinct().Order().ToArray(),character));
        }
        var parameters=CatacombsCreaturesParameters.Build(Read("param/gameparam/gameparam.parambnd.dcx"),source,definitions,owners);files.Add("param/gameparam/gameparam.parambnd.dcx",parameters.Game);
        var used=target.Parts.GetEntries().Select(p=>p.EntityID).Concat(target.Events.GetEntries().Select(p=>p.EntityID)).Concat(target.Regions.GetEntries().Select(p=>p.EntityID)).ToHashSet();var spawns=new List<MapEnemySpawns.Spawn>();
        foreach(var f in Families) {
            string model="c"+f.Model;var visual=(MSBB.Model.Enemy)donorMap.Models.Enemies.Single(m=>m.Name=="c2610").DeepCopy();visual.Name=model;visual.SibPath=visual.SibPath.Replace("c2610",model,StringComparison.OrdinalIgnoreCase);target.Models.Enemies.Add(visual);var skin=skins[f.Model].Skin;
            foreach(var actor in plan.Ordinary.Where(p=>p.NPCParamID==f.Npc)) {
                int entity=4058000+spawns.Count;Need(entity<4059000&&used.Add(entity),"owned actor occupied");int slot=MapCollisionBindings.ObjectSlot(chunks,actor.CollisionName,actor.Position+offset);
                var p=(MSBB.Part.Enemy)donor.DeepCopy();p.Name=model+"_catacombs_"+spawns.Count.ToString("D4");p.ModelName=model;p.EntityID=entity;p.NPCParamID=parameters.Npcs[(f.Model,actor.ThinkParamID)];p.ThinkParamID=parameters.Thinks[actor.ThinkParamID];p.TalkID=-1;p.CharaInitID=-1;p.Position=actor.Position+offset;p.Rotation=actor.Rotation;p.Scale=actor.Scale;
                p.CollisionName=slots[slot].Name;p.InitAnimID=actor.InitAnimID==9001?7004:-1;p.DamageAnimID=-1;p.PatrolInfoName=null!;p.PatrolType=0;p.FallbackPlatoonID=0;Array.Clear(p.MovePointNames);Array.Fill(p.DrawGroups,uint.MaxValue);Array.Fill(p.DispGroups,uint.MaxValue);Array.Fill(p.BackreadGroups,uint.MaxValue);target.Parts.Enemies.Add(p);
                spawns.Add(new(actor.Name,actor.ModelName,actor.NPCParamID,actor.ThinkParamID,actor.EntityID,model,entity,p.NPCParamID,p.ThinkParamID,p.Position,masks[f.Model].Order().ToArray(),skin.DirectWeightShare,1,skin.Bones,false,actor.InitAnimID,"Catacombs source rig/c2610"));
            }
        }
        string aiPath="script/"+targetMap+".luabnd.dcx";var ai=CatacombsCreaturesAi.Build(Read(aiPath),source,native);files.Add(aiPath,ai);foreach(var (p,b) in MapNativeAi.Fallbacks(ai,staged))files.TryAdd(p,b);
        string sfxPath="sfx/frpg_sfxbnd_"+targetMap[..3]+".ffxbnd.dcx";var fire=CatacombsCreaturesEffects.Compose(Read(sfxPath),native(CatacombsCreaturesEffects.NativePath));var arrow=NativeEffectClosure.Compose(fire.Bytes,native(HollowProjectiles.EffectsPath),HollowProjectiles.EffectIDs);files.Add(sfxPath,arrow.Bytes);
        var script=CatacombsCreaturesEvents.Apply(scriptBytes,plan,spawns);var byId=spawns.Where(p=>p.SourceEntityID>=0).ToDictionary(p=>p.SourceEntityID);
        var cohorts=plan.Cohorts.Select(c=>new Cohort(c.Slot,c.Caster.EntityID,byId[c.Caster.EntityID].EntityID,CatacombsCreaturesEvents.CasterFlag(c.Slot),c.Skeletons.Select(p=>byId[p.EntityID].EntityID).ToArray())).ToArray();
        return new(files,script,spawns,cohorts,plan.Excluded.Select(p=>new MapEnemySpawns.Omitted(p.Name,p.ModelName,p.NPCParamID,p.InitAnimID,"Conditional phantom, boss/helper or unsupported event-owned Catacombs actor; not enabled as an ordinary fresh-game enemy.")).ToArray(),[
            "63 ordinary source actors comprise49 necromancer-linked skeletons,6 nonrespawning necromancers,1 giant skeleton and7 bonewheels. Eight skeleton and three wheel Gravelord phantoms remain disabled; Pinwheel/illusion helpers and unsupported event populations require their own adapters.",
            "Source weapon-family animation, source AI helper closure, attacks, contact sockets, base statistics and resident scaling use owned rows. Source-sized hurt bodies, child joints and complete source skeleton mappings follow the visible source rig. Native controller locomotion, action duration, solver behavior and sound remain approximations; fixture packages do not prove gameplay.",
            "Seventeen source proximity wakeups retain their authored distances. While its caster lives, a skeleton clamps at1HP without death rewards, plays source lethal collapse9000, a5-second pile interval and source reassembly9060 with an owned full heal. This is an explicit native event approximation of DS1 state143/144; caster death disables immortality and persists in known native flag storage.",
            "Necromancers preserve audited source26500/26501 ballistics and use complete native priest small-fire FX closure. Skeleton bows use native Bowblade arrow visuals/ballistics with source ranged damage and source muzzle timing. Effects, resurrection, AI navigation and final loot require runtime validation. Existing destination variants of unrelated shared native resources are retained and listed separately.",
            "Source physical break thresholds/removal policies remain unported; source child joints use verified native constraint carriers. Source c2900 a99 damage reactions use the native no-override policy. Collapse/reassembly animations are separate from full physical bone separation; see per-model physics reports."],fire.SharedVariants.Concat(arrow.SharedVariants).Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal).ToArray(),skins.ToDictionary(p=>"c"+p.Key,p=>p.Value.Physics??throw new InvalidDataException("Core Catacombs physics missing")));
    }
}

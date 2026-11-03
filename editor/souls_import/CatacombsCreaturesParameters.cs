// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class CatacombsCreaturesParameters {
    internal const int Heal=886900;
    internal sealed record Owner(int Model,int SourceNpc,IReadOnlyList<int> SourceThinks,CatacombsCreaturesCharacters.Result Character,int NativeNpc=261010,int NativeThink=261010,IReadOnlyList<int>? ControllerEffects=null,bool SourceRagdoll=false);
    internal sealed record Result(byte[] Game,IReadOnlyDictionary<(int,int),int> Npcs,IReadOnlyDictionary<int,int> Thinks,IReadOnlyList<int> Effects);
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs parameters: "+why);}
    internal static Result Build(byte[] gameBytes,Func<string,byte[]?> source,string definitions,IReadOnlyList<Owner> owners,IReadOnlyDictionary<int,int>? goalMap=null,int thinkFirst=884100,int effectFirst=886800,bool createHeal=true,IReadOnlyDictionary<int,int>? projectileEffects=null) {
        var game=BND4.Read(gameBytes);var original=BND3.Read(source("param/GameParam/GameParam.parambnd.dcx")!);
        PARAMDEF Definition(string g,string n)=>PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../"+g+"/Defs/"+n+".xml")));
        string DefName(string n)=>n switch {"AtkParam_Npc"=>"AtkParam","SpEffectParam"=>"SpEffect","Bullet"=>"BulletParam",_=>n};
        BinderFile File(string name)=>game.Files.Single(f=>Leaf(f.Name)==name+".param");
        var raw=new Dictionary<string,PARAM>();var native=new Dictionary<string,PARAM>();var authored=new Dictionary<string,PARAM>();
        PARAM Raw(string n){if(raw.TryGetValue(n,out var p))return p;var b=File(n).Bytes;var h=PARAM.Read(b);p=ImportParam.Read(b,h.ParamType,(int)h.DetectedSize,h.ParamdefDataVersion);raw.Add(n,p);return p;}
        PARAM Native(string n){if(native.TryGetValue(n,out var p))return p;p=PARAM.Read(File(n).Bytes);p.ApplyParamdef(Definition("BB",DefName(n)));native.Add(n,p);return p;}
        PARAM Source(string n){if(authored.TryGetValue(n,out var p))return p;p=PARAM.Read(original.Files.Single(f=>Leaf(f.Name)==n+".param").Bytes);p.ApplyParamdef(Definition("DS1R",DefName(n)));authored.Add(n,p);return p;}
        int I(PARAM.Row r,string f)=>Convert.ToInt32(r[f].Value);
        void Set(PARAM.Row r,string f,object v){var c=r.Cells.Single(c=>c.Def.InternalName==f);c.Value=Convert.ChangeType(v,c.Value.GetType(),System.Globalization.CultureInfo.InvariantCulture);}
        void Copy(PARAM.Row from,PARAM.Row to,IEnumerable<string> fields){foreach(string f in fields){var a=from[f];var b=to[f];Need(a.Def.DisplayType==b.Def.DisplayType&&a.Def.BitSize==b.Def.BitSize&&a.Def.ArrayLength==b.Def.ArrayLength,"field ABI changed: "+f);Set(to,f,a.Value);}}
        string[] Shared(PARAM.Row from,PARAM.Row to)=>from.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)&&c.Def.ArrayLength==1&&to.Cells.Any(n=>n.Def.InternalName==c.Def.InternalName&&n.Def.DisplayType==c.Def.DisplayType&&n.Def.BitSize==c.Def.BitSize&&n.Def.ArrayLength==1)).Select(c=>c.Def.InternalName).ToArray();
        byte[] Encode(string n,PARAM.Row row){var p=PARAM.Read(File(n).Bytes);p.ApplyParamdef(Definition("BB",DefName(n)));p.Rows.Clear();p.Rows.Add(row);p.Rows.Add(new PARAM.Row(row){ID=row.ID+1});var b=p.Write();var h=PARAM.Read(b);return ImportParam.Payload(ImportParam.Read(b,h.ParamType,(int)h.DetectedSize,h.ParamdefDataVersion).Rows[0]);}
        void Add(string n,int donor,int id,string label,Action<PARAM.Row> edit,bool neutral=false){var r=neutral?new PARAM.Row(id,label,Definition("BB",DefName(n))):new PARAM.Row(Native(n).Rows.Single(r=>r.ID==donor)){ID=id,Name=label};var before=Encode(n,r);edit(r);var after=Encode(n,r);var table=Raw(n);var d=table.Rows.Single(r=>r.ID==donor);var bytes=ImportParam.Payload(d);Need(bytes.Length==before.Length&&bytes.Length==after.Length,"owned row ABI size changed");if(neutral)bytes=after;else for(int i=0;i<bytes.Length;i++){int mask=before[i]^after[i];Need(((before[i]^bytes[i])&mask)==0,"opaque edited field mismatch "+n);bytes[i]=(byte)((bytes[i]&~mask)|(after[i]&mask));}ImportParam.Append(table,d,id,label,bytes);}
        var thinks=new Dictionary<int,int>();var npcs=new Dictionary<(int,int),int>();
        foreach(int id in owners.SelectMany(o=>o.SourceThinks).Distinct().Order()) {
            var brain=Source("NpcThinkParam").Rows.Single(r=>r.ID==id);int target=thinkFirst+thinks.Count;thinks.Add(id,target);
            Add("NpcThinkParam",owners.First(o=>o.SourceThinks.Contains(id)).NativeThink,target,"Imported Catacombs source AI "+id,to=>{Copy(brain,to,Shared(brain,to));Need(I(brain,"logicId")==10000,"source logic changed");Set(to,"logicId",10000);Set(to,"battleGoalID",goalMap==null?CatacombsCreaturesAi.Goal(I(brain,"battleGoalID")):goalMap[I(brain,"battleGoalID")]);foreach(string f in new[]{"eyeDist_forDark","eyeDist_forPitchDark"})Set(to,f,brain["eye_dist"].Value);foreach(string f in new[]{"battleStartDist_forDark","battleStartDist_forPitchDark"})Set(to,f,brain["BattleStartDist"].Value);});
        }
        // Source 143/144 skeleton fake-death state codes have no installed BB
        // rows. The event adapter owns temporary defeat; only ordinary stat
        // effects are cloned, with recursive resident references rejected.
        PARAM.Row Behavior(int npc,int judge){int variation=I(Source("NpcParam").Rows.Single(n=>n.ID==npc),"behaviorVariationId");var found=Source("BehaviorParam").Rows.Where(r=>I(r,"variationId")==variation&&I(r,"behaviorJudgeId")==judge).ToArray();Need(found.Length==1,"missing/ambiguous source behavior "+npc+"/"+variation+"/"+judge);return found[0];}
        var contactBehaviors=owners.SelectMany(o=>o.Character.Contacts.Select(c=>Behavior(o.SourceNpc,c.Judge))).DistinctBy(r=>r.ID).ToArray();
        var contactAttacks=contactBehaviors.Select(r=>I(r,"refType")==0?I(r,"refId"):I(Source("Bullet").Rows.Single(b=>b.ID==I(r,"refId")),"atkId_Bullet")).Where(id=>id>0).Distinct().Select(id=>Source("AtkParam_Npc").Rows.Single(r=>r.ID==id)).ToArray();
        var attackEffects=contactAttacks.SelectMany(a=>Enumerable.Range(0,5).Select(i=>I(a,"spEffectId"+i))).Where(id=>id>0).Distinct().ToArray();
        Need(attackEffects.All(id=>id is 5270 or 5271),"unverified source attack status family");
        var effectSources=owners.Select(o=>Source("NpcParam").Rows.Single(r=>r.ID==o.SourceNpc)).SelectMany(n=>Enumerable.Range(0,8).Select(i=>I(n,"spEffectID"+i)).Append(I(n,"GameClearSpEffectID"))).Where(id=>id>0&&id!=5451).Concat(attackEffects).Distinct().Order().ToArray();
        var effects=effectSources.Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>effectFirst+p.n);Need(effects.Count<100,"resident effect budget");
        int effectDonor=(int)Raw("SpEffectParam").Rows.First().ID;
        foreach(var (id,owned) in effects) {var from=Source("SpEffectParam").Rows.Single(r=>r.ID==id);Add("SpEffectParam",effectDonor,owned,"Imported Catacombs source effect "+id,to=>{Copy(from,to,Shared(from,to));foreach(string f in new[]{"replaceSpEffectId","cycleOccurrenceSpEffectId","atkOccurrenceSpEffectId"})Need(I(from,f)==-1,"recursive source resident effect");
            bool hostile=attackEffects.Contains(id);var policy=Native("SpEffectParam").Rows.SingleOrDefault(r=>r.ID==id)??Native("SpEffectParam").Rows.Single(r=>r.ID==5300);
            Need(hostile?I(policy,"effectTargetOpposeTarget")==1&&I(policy,"effectTargetSelfTarget")==0:I(from,"effectTargetSelf")==1&&I(policy,"effectTargetSelfTarget")==1,"native effect recipient policy changed "+id);
            foreach(string field in new[]{"effectTargetOpposeTarget","effectTargetFriendlyTarget","effectTargetSelfTarget"})Set(to,field,policy[field].Value);
        },true);}
        if(createHeal) {
        var heal=Source("SpEffectParam").Rows.Single(r=>r.ID==5450);Need(Convert.ToSingle(heal["changeHpRate"].Value)==-100&&Convert.ToSingle(heal["effectEndurance"].Value)==0&&I(heal,"stateInfo")==0,"source full heal changed");
        var nativeHeal=Native("SpEffectParam").Rows.Single(r=>r.ID==5625);Need(Convert.ToSingle(nativeHeal["changeHpRate"].Value)==-100&&I(nativeHeal,"effectTargetSelf")==1&&I(nativeHeal,"effectTargetAI")==1&&I(nativeHeal,"effectTargetSelfTarget")==1,"native NPC resurrection heal policy changed");
        Add("SpEffectParam",5625,Heal,"Imported Catacombs resurrection heal",to=>Copy(heal,to,Shared(heal,to)));
        }
        var behavior=Source("BehaviorParam");var attacks=Source("AtkParam_Npc");var bullets=Source("Bullet");
        foreach(var owner in owners) {
            var from=Source("NpcParam").Rows.Single(r=>r.ID==owner.SourceNpc);int variation=owner.Model*10;int sourceVariation=I(from,"behaviorVariationId");
            foreach(int think in owner.SourceThinks.Order()) {int id=owner.Model*100+npcs.Keys.Count(k=>k.Item1==owner.Model);npcs.Add((owner.Model,think),id);Add("NpcParam",owner.NativeNpc,id,"Imported Catacombs creature "+owner.Model+"/"+think,to=> {
                Copy(from,to,new[]{"turnVellocity","hitHeight","hitRadius","weight","hitYOffset","hp","mp","getSoul","physGuardCutRate","magGuardCutRate","fireGuardCutRate","thunGuardCutRate","stamina","staminaRecoverBaseVel","def_phys","def_slash","def_blow","def_thrust","def_mag","def_fire","def_thunder","resist_poison","resist_desease","resist_blood","resist_curse","superArmorDurability","knockbackRate","fallDamageDump","isNoDamageMotion","isUnduration","disableRespawn"});
                Set(to,"behaviorVariationId",variation);Set(to,"aiThinkId",thinks[think]);Set(to,"teamType",23);Set(to,"npcType",0);Set(to,"normalChangeAnimChrId",-1);Set(to,"normalChangeTexChrId",-1);
                for(int i=0;i<8;i++)Set(to,"spEffectID"+i,effects.GetValueOrDefault(I(from,"spEffectID"+i),-1));Set(to,"GameClearSpEffectID",effects.GetValueOrDefault(I(from,"GameClearSpEffectID"),-1));for(int i=1;i<=6;i++)Set(to,"itemLotId_"+i,-1);
                if(owner.SourceRagdoll)Set(to,"useRagdoll",from["useRagdoll"].Value);
                if(owner.ControllerEffects is {Count:>0}) {var donor=Native("NpcParam").Rows.Single(r=>r.ID==owner.NativeNpc);var unused=Enumerable.Range(0,8).Where(i=>I(from,"spEffectID"+i)<=0).ToArray();Need(unused.Length>=owner.ControllerEffects.Count,"source has no empty controller effect slots");for(int n=0;n<owner.ControllerEffects.Count;n++){int effect=owner.ControllerEffects[n];Need(Enumerable.Range(0,8).Any(i=>I(donor,"spEffectID"+i)==effect),"native controller effect not resident on donor");Set(to,"spEffectID"+unused[n],effect);}}
                // Masks already selected and compacted while baking source meshes.
                for(int i=0;i<32;i++)Set(to,"modelDispMask"+i,1);
            });}
            var rows=owner.Character.Contacts.Select(c=>Behavior(owner.SourceNpc,c.Judge)).DistinctBy(r=>r.ID).ToArray();
            var rootBullets=rows.Where(r=>I(r,"refType")==1).Select(r=>I(r,"refId")).Distinct().ToArray();var bulletIds=rootBullets.Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>88400000+(owner.Model-8840)*1000+p.n);
            var sourceAttackIds=rows.Where(r=>I(r,"refType")==0).Select(r=>I(r,"refId")).Concat(rootBullets.Select(id=>I(bullets.Rows.Single(r=>r.ID==id),"atkId_Bullet"))).Where(id=>id>0).Distinct().Order().ToArray();var attackIds=sourceAttackIds.Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>8840000+(owner.Model-8840)*1000+p.n);
            foreach(var (sourceId,owned) in attackIds) {
                var damage=attacks.Rows.Single(r=>r.ID==sourceId);Need(I(damage,"throwFlag")==0&&I(damage,"throwTypeId")==0,"source paired attack needs a dedicated player adapter");
                int donor=Native("AtkParam_Npc").Rows.Any(r=>r.ID==sourceId)?sourceId:261000;
                Add("AtkParam_Npc",donor,owned,"Imported Catacombs attack "+owner.Model+"/"+sourceId,to=> {
                    Copy(damage,to,Shared(damage,to));Need(I(damage,"atkAttribute") is >=0 and <=4,"unverified damage category "+sourceId+"="+I(damage,"atkAttribute"));Set(to,"atkAttribute",I(damage,"atkAttribute") is 2 or 3?I(damage,"atkAttribute"):1);
                    for(int i=0;i<5;i++){int effect=I(damage,"spEffectId"+i);Set(to,"spEffectId"+i,effect>0?effects[effect]:-1);}
                    for(int i=0;i<4;i++)foreach(int n in new[]{1,2}){string f=$"hit{i}_DmyPoly{n}";int dummy=I(damage,f);bool active=Convert.ToSingle(damage[$"hit{i}_Radius"].Value)>0;Set(to,f,active&&dummy>=0?CatacombsCreaturesCharacters.ContactBase+dummy:-1);}
                });
            }
            foreach(var (sourceId,owned) in bulletIds) {
                var fromBullet=bullets.Rows.Single(r=>r.ID==sourceId);Need(I(fromBullet,"HitBulletID")==-1,"source projectile child needs closure ownership");
                int donor=sourceId==29000?HollowProjectiles.Arrow:sourceId;
                if(sourceId!=29000){var legacy=Native("Bullet").Rows.Single(r=>r.ID==sourceId);foreach(var c in fromBullet.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)))Need(legacy[c.Def.InternalName].Value.Equals(c.Value),"native legacy source projectile differs");}
                Add("Bullet",donor,owned,"Imported Catacombs projectile "+owner.Model+"/"+sourceId,to=> {
                    Set(to,"atkId_Bullet",attackIds[I(fromBullet,"atkId_Bullet")]);
                    if(projectileEffects!=null)foreach(string field in new[]{"sfxId_Bullet","sfxId_Hit","sfxId_Flick"}) {int fx=I(fromBullet,field);if(fx>0){Need(projectileEffects.TryGetValue(fx,out int nativeFx),"unmapped owned projectile effect "+fx);Set(to,field,nativeFx);}}
                    if(sourceId==26500){Set(to,"sfxId_Bullet",CatacombsCreaturesEffects.FireFlight);Set(to,"sfxId_Hit",CatacombsCreaturesEffects.FireHit);}
                    if(sourceId==29000){/* Explicit native Bowblade visual/ballistic adapter. */}
                    foreach(string f in new[]{"spEffectIDForShooter","autoSearchNPCThinkID","spEffectId0","spEffectId1","spEffectId2","spEffectId3","spEffectId4"})Need(I(fromBullet,f)<=0,"projectile has unowned effect/AI");
                });
            }
            foreach(var row in rows){int judge=I(row,"behaviorJudgeId"),kind=I(row,"refType");Need(kind is 0 or 1,"unsupported source behavior reference");int donor=Native("BehaviorParam").Rows.Any(r=>r.ID==row.ID)?(int)row.ID:226100100;Add("BehaviorParam",donor,200000000+variation*1000+judge,"Imported Catacombs behavior "+owner.Model+"/"+judge,to=>{Copy(row,to,Shared(row,to));Set(to,"variationId",variation);Set(to,"refId",kind==0?attackIds[I(row,"refId")]:bulletIds[I(row,"refId")]);});}
        }
        foreach(var (name,table) in raw)File(name).Bytes=ImportParam.Write(table);
        return new(game.Write(DCX.Type.DCX_DFLT_10000_44_9),npcs,thinks,effects.Values.ToArray());
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

internal static class GoldenGolemParameters {
    internal const int Npc=883200,Think=883200,Goal=883200,Variation=88320;
    internal static readonly int[] RequiredNativeSfx=[625307];
    internal sealed record Ids(int Npc,int Think,int Goal,int Variation,IReadOnlyDictionary<int,int> Behaviors,IReadOnlyDictionary<int,int> Attacks,IReadOnlyDictionary<int,int> Bullets,IReadOnlyDictionary<int,int> Effects);
    internal sealed record Report(Ids Ids,int SourceVariation,IReadOnlyList<int> Judges,IReadOnlyList<int> ProjectileRoots,IReadOnlyList<string> Limitations);
    internal sealed record Result(byte[] Game,Report Report) {internal Ids Ids=>Report.Ids;}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Golden golem parameters: "+why);}
    internal static Result Build(byte[] gameBytes,Func<string,byte[]?> source,string definitions,SourceCharacterActions actions,IReadOnlySet<int> sockets) {
        var game=BND4.Read(gameBytes);var original=BND3.Read(source("param/GameParam/GameParam.parambnd.dcx")??throw new FileNotFoundException("Source Golden parameters unavailable"));
        PARAMDEF Definition(string g,string name)=>PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../"+g+"/Defs/"+name+".xml")));
        BinderFile File(string name)=>game.Files.Single(f=>Leaf(f.Name)==name+".param");
        var raw=new Dictionary<string,PARAM>();var typed=new Dictionary<string,PARAM>();var authored=new Dictionary<string,PARAM>();
        string DefName(string name)=>name switch{"AtkParam_Npc"=>"AtkParam","SpEffectParam"=>"SpEffect","Bullet"=>"BulletParam",_=>name};
        PARAM Raw(string name){if(raw.TryGetValue(name,out var p))return p;var f=File(name);var h=PARAM.Read(f.Bytes);p=ImportParam.Read(f.Bytes,h.ParamType,checked((int)h.DetectedSize),h.ParamdefDataVersion);raw.Add(name,p);return p;}
        PARAM Native(string name){if(typed.TryGetValue(name,out var p))return p;p=PARAM.Read(File(name).Bytes);p.ApplyParamdef(Definition("BB",DefName(name)));typed.Add(name,p);return p;}
        PARAM Source(string name){if(authored.TryGetValue(name,out var p))return p;p=PARAM.Read(original.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(Definition("DS1R",DefName(name)));authored.Add(name,p);return p;}
        void Set(PARAM.Row row,string field,object value){var c=row.Cells.Single(c=>c.Def.InternalName==field);c.Value=Convert.ChangeType(value,c.Value.GetType(),System.Globalization.CultureInfo.InvariantCulture);}
        void Copy(PARAM.Row from,PARAM.Row to,IEnumerable<string> names){foreach(string name in names){var a=from.Cells.Single(c=>c.Def.InternalName==name);var b=to.Cells.Single(c=>c.Def.InternalName==name);Need(a.Def.DisplayType==b.Def.DisplayType&&a.Def.BitSize==b.Def.BitSize&&a.Def.ArrayLength==b.Def.ArrayLength,"typed field ABI: "+name);Set(to,name,a.Value);}}
        byte[] Encode(string name,PARAM.Row row){var p=PARAM.Read(File(name).Bytes);p.ApplyParamdef(Definition("BB",DefName(name)));p.Rows.Clear();p.Rows.Add(row);p.Rows.Add(new PARAM.Row(row){ID=row.ID+1});var b=p.Write();var h=PARAM.Read(b);return ImportParam.Payload(ImportParam.Read(b,h.ParamType,checked((int)h.DetectedSize),h.ParamdefDataVersion).Rows[0]);}
        void Add(string name,int donor,int id,string label,Action<PARAM.Row> edit,bool neutral=false) {
            var table=Raw(name);var donorRaw=table.Rows.Single(r=>r.ID==donor);var row=neutral?new PARAM.Row(id,label,Definition("BB",DefName(name))):new PARAM.Row(Native(name).Rows.Single(r=>r.ID==donor)){ID=id,Name=label};
            if(neutral){edit(row);ImportParam.Append(table,donorRaw,id,label,Encode(name,row));return;}
            var before=Encode(name,row);edit(row);var after=Encode(name,row);var data=ImportParam.Payload(donorRaw);
            Need(before.Length==after.Length&&before.Length==data.Length,"native owned row ABI: "+name);
            for(int i=0;i<data.Length;i++){int mask=before[i]^after[i];Need(((before[i]^data[i])&mask)==0,"opaque native edited-field mismatch: "+name);data[i]=(byte)((data[i]&~mask)|(after[i]&mask));}
            ImportParam.Append(table,donorRaw,id,label,data);
        }
        int I(PARAM.Row r,string f)=>Convert.ToInt32(r[f].Value);
        string[] Shared(PARAM.Row from,PARAM.Row to)=>from.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)&&c.Def.ArrayLength==1&&to.Cells.Any(n=>n.Def.InternalName==c.Def.InternalName&&n.Def.DisplayType==c.Def.DisplayType&&n.Def.BitSize==c.Def.BitSize&&n.Def.ArrayLength==1)).Select(c=>c.Def.InternalName).ToArray();
        var npc=Source("NpcParam").Rows.Single(r=>r.ID==271100);var brain=Source("NpcThinkParam").Rows.Single(r=>r.ID==271100);int sourceVariation=I(npc,"behaviorVariationId");Need(sourceVariation==27110,"actual golden variation changed");
        var sourceBehavior=Source("BehaviorParam");var sourceAtk=Source("AtkParam_Npc");var sourceBullets=Source("Bullet");
        var judges=Enumerable.Range(3000,9).SelectMany(id=>actions.Resolve(id).Events).Where(e=>e.Type is 1 or 2).Select(e=>BitConverter.ToInt32(e.GetParameterBytes(false),8)).Distinct().Order().ToArray();
        Need(judges.SequenceEqual(new[]{100,110,120,130,140,141,150,160,170,180}),"nine golden attack judges changed");
        var behaviorRows=judges.ToDictionary(j=>j,j=>sourceBehavior.Rows.Single(r=>I(r,"variationId")==sourceVariation&&I(r,"behaviorJudgeId")==j));
        var roots=behaviorRows.Values.Where(r=>I(r,"refType")==1).Select(r=>I(r,"refId")).Distinct().Order().ToArray();
        Need(roots.SequenceEqual(new[]{27110}),"golden projectile root changed");var bullets=new HashSet<int>();var pending=new Queue<int>(roots);
        while(pending.TryDequeue(out int id)){if(!bullets.Add(id))continue;Need(bullets.Count<=32,"projectile closure budget");int child=I(sourceBullets.Rows.Single(r=>r.ID==id),"HitBulletID");if(child>=0)pending.Enqueue(child);}
        Need(bullets.Order().SequenceEqual(Enumerable.Range(27110,5)),"golden ground-wave closure changed");
        var attackSources=behaviorRows.Values.Where(r=>I(r,"refType")==0).Select(r=>I(r,"refId")).Concat(bullets.Select(id=>I(sourceBullets.Rows.Single(r=>r.ID==id),"atkId_Bullet"))).Where(id=>id>0).Distinct().Order().ToArray();
        var attackIds=attackSources.Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>88320000+p.n);var bulletIds=bullets.Order().Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>88320500+p.n);
        var behaviors=behaviorRows.Values.ToDictionary(r=>checked((int)r.ID),r=>checked(200000000+Variation*1000+I(r,"behaviorJudgeId")));
        var effectSources=Enumerable.Range(0,8).Select(i=>I(npc,"spEffectID"+i)).Append(I(npc,"GameClearSpEffectID")).Where(id=>id>0).Distinct().Order().ToArray();
        Need(effectSources.SequenceEqual(new[]{5320,5360,7005,7405,71110,90001}),"golden resident profile changed");var effects=effectSources.Select((id,n)=>(id,n)).ToDictionary(p=>p.id,p=>883300+p.n);
        var ids=new Ids(Npc,Think,Goal,Variation,behaviors,attackIds,bulletIds,effects);
        foreach(int id in attackSources) {
            var from=sourceAtk.Rows.Single(r=>r.ID==id);Need(I(from,"throwFlag")==0&&I(from,"throwTypeId")==0,"paired attack unsupported");int donor=Native("AtkParam_Npc").Rows.Any(r=>r.ID==id)?id:271000;
            Add("AtkParam_Npc",donor,attackIds[id],"Imported golden golem attack "+id,to=> {
                Copy(from,to,Shared(from,to));Need(I(from,"atkAttribute") is 0 or 4,"unverified golden attack category");Set(to,"atkAttribute",1);
                for(int i=0;i<5;i++){Need(I(from,"spEffectId"+i)<=0,"unowned golden attack status");Set(to,"spEffectId"+i,-1);}
                for(int i=0;i<4;i++)foreach(int n in new[]{1,2}) {string field=$"hit{i}_DmyPoly{n}";int dummy=I(from,field);bool active=Convert.ToSingle(from[$"hit{i}_Radius"].Value)>0;
                    Need(!active||dummy<0||sockets.Contains(dummy),"source contact socket missing "+dummy);Set(to,field,active&&dummy>=0?GoldenGolemCharacters.Socket(dummy):-1);}
            });
        }
        foreach(int id in bullets.Order()) {
            var from=sourceBullets.Rows.Single(r=>r.ID==id);var legacy=Native("Bullet").Rows.Single(r=>r.ID==id);
            foreach(var c in from.Cells.Where(c=>!c.Def.InternalName.StartsWith("pad",StringComparison.Ordinal)))Need(legacy[c.Def.InternalName].Value.Equals(c.Value),"native legacy golden bullet differs "+id+"/"+c.Def.InternalName);
            Add("Bullet",id,bulletIds[id],"Imported golden golem ground-wave "+id,to=> {
                int attack=I(from,"atkId_Bullet"),child=I(from,"HitBulletID");Set(to,"atkId_Bullet",attack>0?attackIds[attack]:attack);Set(to,"HitBulletID",child>=0?bulletIds[child]:child);
                foreach(string f in new[]{"sfxId_Bullet","sfxId_Hit","sfxId_Flick"}) {int fx=I(from,f);Need(fx is -1 or 0 or 12711,"unverified golden projectileFX");if(fx>0)Set(to,f,625307);}
                foreach(string f in new[]{"spEffectIDForShooter","autoSearchNPCThinkID","spEffectId0","spEffectId1","spEffectId2","spEffectId3","spEffectId4"})Need(I(from,f)<=0,"unowned projectile status/AI");
            });
        }
        foreach(var (judge,from) in behaviorRows)Add("BehaviorParam",227100100,behaviors[(int)from.ID],"Imported golden golem behavior "+judge,to=> {
            Copy(from,to,Shared(from,to));Set(to,"variationId",Variation);int kind=I(from,"refType");Need(kind is 0 or 1,"unsupported behavior reference");Set(to,"refId",kind==0?attackIds[I(from,"refId")]:bulletIds[I(from,"refId")]);
        });
        int effectDonor=(int)Raw("SpEffectParam").Rows.First().ID;
        foreach(var (id,owned) in effects) {var from=Source("SpEffectParam").Rows.Single(r=>r.ID==id);
            foreach(string f in new[]{"replaceSpEffectId","cycleOccurrenceSpEffectId","atkOccurrenceSpEffectId"})Need(I(from,f)==-1,"resident effect reference requires ownership");
            Add("SpEffectParam",effectDonor,owned,"Imported golden golem resident "+id,to=>{Copy(from,to,Shared(from,to));Set(to,"effectTargetSelfTarget",1);},neutral:true);
        }
        Add("NpcParam",273101,Npc,"Imported Golden Golem c2711",to=> {
            Copy(npc,to,new[]{"turnVellocity","hitHeight","hitRadius","hitYOffset","weight","hp","mp","getSoul","physGuardCutRate","magGuardCutRate","fireGuardCutRate","thunGuardCutRate","stamina","staminaRecoverBaseVel","def_phys","def_slash","def_blow","def_thrust","def_mag","def_fire","def_thunder","resist_poison","resist_desease","resist_blood","resist_curse","superArmorDurability","knockbackRate","fallDamageDump","useRagdoll","useRagdollCamHit","disableClothRigidHit","isNoDamageMotion","isUnduration","disableRespawn","isEnableNeckTurn"});
            Set(to,"behaviorVariationId",Variation);Set(to,"aiThinkId",Think);Set(to,"teamType",23);Set(to,"npcType",0);Set(to,"normalChangeAnimChrId",-1);Set(to,"normalChangeTexChrId",-1);
            for(int i=0;i<8;i++)Set(to,"spEffectID"+i,effects.GetValueOrDefault(I(npc,"spEffectID"+i),-1));Set(to,"GameClearSpEffectID",effects[I(npc,"GameClearSpEffectID")]);for(int i=1;i<=6;i++)Set(to,"itemLotId_"+i,-1);
            for(int i=0;i<32;i++)Set(to,"modelDispMask"+i,1);
        });
        Add("NpcThinkParam",273100,Think,"Imported golden golem source AI",to=> {
            Copy(brain,to,Shared(brain,to));Need(I(brain,"logicId")==10000&&I(brain,"battleGoalID")==271100,"source logic routing changed");Set(to,"logicId",10000);Set(to,"battleGoalID",Goal);
            foreach(string f in new[]{"eyeDist_forDark","eyeDist_forPitchDark"})Set(to,f,brain["eye_dist"].Value);foreach(string f in new[]{"battleStartDist_forDark","battleStartDist_forPitchDark"})Set(to,f,brain["BattleStartDist"].Value);
        });
        foreach(var (name,table) in raw)File(name).Bytes=ImportParam.Write(table);
        return new(game.Write(DCX.Type.DCX_DFLT_10000_44_9),new(ids,sourceVariation,judges,roots,[
            "Actual golden variation27110 and its nine source attacks are owned; the five-stage27110ground wave retains every source ballistic field and child link. Native625307impact replaces unconverted source12711crystal appearance.",
            "All source resident stats and damage-motion immunities are private rows, with BB's explicit resident self-target gate. Quest placement/persistence belongs to MapDuskEvents. Native gameplay remains a separate acceptance check."]));
    }
}

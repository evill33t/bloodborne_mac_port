// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// Verified ordinary Darkroot foliage. Event-controlled ambushes and the paired
// grab remain separate adapters; they are never enabled by a broad model match.
internal static class FoliageCombat {
    internal static readonly NonHumanoidEnemies.Profile Profile=new("c2330","c2050",205010,205010,"m22_00_00_00","aicommon",72,140);
    internal const int ContactBase=100;
    static readonly HashSet<string> Ordinary=["c2330_0000","c2330_0001","c2330_0002","c2330_0012","c2330_0013","c2330_0014","c2330_0015","c2330_0016","c2330_0017","c2330_0018","c2330_0020","c2330_0022"];
    static readonly int[] Actions=[3002,3003,3005,3006],Judges=[120,130,150,151,160,161];
    internal static bool Select(MSB1.Part.Enemy p,string map)=>map=="m12_00_00_00"&&p.ModelName=="c2330"&&Ordinary.Contains(p.Name)&&
        p.EntityID==-1&&p.NPCParamID==(p.Name is "c2330_0000" or "c2330_0001" or "c2330_0002"?233000:233001)&&
        p.ThinkParamID==233000&&p.InitAnimID==-1&&p.CharaInitID==-1&&p.TalkID<=0;
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Require(bool b,string why){if(!b)throw new InvalidDataException("Foliage combat: "+why);}
    internal static int SourceSlot(int id)=>id switch {
        0 or 200 or 201 or 202 or 203 or 500 or 602 or 603 or 604 or 605 or 701 or 702 or 703 or
        1500 or 1750 or 2000 or 2001 or 2002 or 2004 or 2005 or 2006 or 2007 or 2010 or 2011 or 2012 or 2013 or
        2014 or 2020 or 2030 or 2031 or 2032 or 2040 or 2050 or 2051 or 2052 or 2053 or
        2200 or 2202 or 2203 or 2205 or 2207 or 2209 or 2250 or 3002 or 3003 or 3005 or 3006 or 3500 or 3501=>id,
        501 or 502 or 503=>500,700=>701,1510 or 1520 or 1521=>1500,1610 or 1620 or 1621=>2200,
        1751 or 1760 or 1770 or 1771=>1750,2003=>2007,2060=>2014,
        2080=>2010,2081=>2011,2082=>2012,2083=>2013,2100 or 2110=>2004,
        2204 or 2211 or 2220 or 2221=>2200,2255 or 2257 or 2270 or 2271=>2250,
        2400 or 2401=>0,2500 or 2501 or 2502 or 2510 or 2511 or 2512=>2020,
        2600 or 2610 or 2620=>2000,2601 or 2611 or 2621=>2001,2602 or 2612 or 2622=>2002,2603 or 2613 or 2623=>2004,
        3000 or 3009 or 3015=>3002,3001 or 3004 or 3007 or 3010 or 3013 or 3014=>3003,
        3008 or 3012 or 3016=>3006,3011=>3005,
        6200 or 7200 or 7201 or 7202 or 7203 or 7204 or 7210 or 7211 or 7212 or 7213 or 7214 or 7220 or 7221 or 7222 or 7223 or 7224 or
        7230 or 7231 or 7233 or 7235 or 7236 or 7240 or 7241 or 7243 or 7245 or 7246 or 7250 or 7251=>0,
        _=>throw new InvalidDataException("Unverified native foliage playback slot: "+id)
    };
    internal static SkinBake Package(Func<string,byte[]?> source,Func<string,byte[]> native,string target,IReadOnlySet<int> masks,Dictionary<string,byte[]> files) {
        var authored=MapCharacters.Authored(source,"c2330");var mesh=FLVER2.Read(authored.Model);
        var materials=BND3.Read(source("mtd/Mtd.mtdbnd.dcx")??throw new FileNotFoundException("Source foliage material definitions unavailable"));
        var bake=NonHumanoidEnemies.Package(source,native,Profile,target,masks,files,SourceSlot,adaptMaterials:skin=> {
            Require(mesh.Meshes.Count==skin.Model.Meshes.Count,"unverified foliage material-mask ordering");
            for(int i=0;i<mesh.Meshes.Count;i++) {
                string name=Leaf(mesh.Materials[mesh.Meshes[i].MaterialIndex].MTD);
                Require(name is "C_DullLeather[DSB]_Edge_Spec.mtd" or "C_DullLeather[DSB]_Spec.mtd","unverified foliage material");
                var from=MTD.Read(materials.Files.Single(f=>Leaf(f.Name)==name).Bytes);int blend=Convert.ToInt32(from.Params.Single(p=>p.Name=="g_BlendMode").Value);
                Require(blend==(name.Contains("_Edge_",StringComparison.Ordinal)?1:0),"source foliage alpha contract changed");
                var material=skin.Model.Materials[skin.Model.Meshes[i].MaterialIndex];Require(Leaf(material.MTD)=="C[ARSN].mtd","native foliage material ABI changed");
                if(blend==1)material.MTD=material.MTD[..^4]+"_e.mtd";
            }
        },adaptRig:(skin,plan)=> {
            foreach(int id in new[]{1,2,10,11,12,20,21,22}) {
                var dummy=mesh.Dummies.Single(d=>d.ReferenceID==id);short owned=checked((short)(ContactBase+id));
                Require(!skin.Model.Dummies.Any(d=>d.ReferenceID==owned),"source contact dummy reservation occupied");
                skin.Model.Dummies.Add(HollowSourceSkin.RebindSocket(dummy,authored.Model,plan,owned));
            }
        });
        var sourceBank=BND3.Read(source("chr/c2330.anibnd.dcx")!);var from=TAE.Read(sourceBank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
        var bank=BND4.Read(files["chr/"+target+".anibnd.dcx"]);var entry=bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var tae=TAE.Read(entry.Bytes);
        Require(!from.BigEndian&&from.Format==TAE.TAEFormat.DS1&&!tae.BigEndian&&tae.Format==TAE.TAEFormat.BB,"animation event ABI changed");
        var carrier=tae.Animations.Single(a=>a.ID==3002);var hit=carrier.Events.Single(e=>e.Type==1);var gate=carrier.Events.First(e=>e.Type==0);
        Require(hit.GetParameterBytes(false).Length==16&&gate.GetParameterBytes(false).Length==16,"native event carrier changed");
        var hitPrototype=hit.GetClone(false);var gatePrototype=gate.GetClone(false);
        foreach(var action in tae.Animations) {
            bool mapped=Actions.Contains((int)action.ID);
            var hitGroup=mapped?action.Events.First(e=>e.Type==1).Group:null;
            var gateGroups=mapped?action.Events.Where(e=>e.Type==0).GroupBy(e=>BitConverter.ToInt32(e.GetParameterBytes(false))).ToDictionary(g=>g.Key,g=>g.First().Group):[];
            Require(!mapped||hitGroup?.GroupType==1&&action.EventGroups.Contains(hitGroup)&&gateGroups.Count>0&&gateGroups.Values.All(g=>g?.GroupType==0&&action.EventGroups.Contains(g)),"native action contact/gate group is missing");
            Require(!mapped||gateGroups.Values.Distinct().Count()==gateGroups.Count,"native cancel gates share a group across distinct gate IDs");
            // No dormant donor attack/projectile can use its old contacts.
            action.Events.RemoveAll(e=>e.Type is 1 or 2);
            if(action.ID is >=3000 and <4000)action.Events.RemoveAll(e=>e.Type is 0 or 66 or 67 or 96 or 112 or 129);
            if(!mapped)continue;
            var original=from.Animations.Single(a=>a.ID==action.ID);Require(original.MiniHeader is TAE.Animation.AnimMiniHeader.Standard s&&!s.ImportsHKX,"source attack acquired an unverified alias");
            var duration=SourceAnimation.Load(source,"c2330",(int)action.ID).Duration;var p=new NativeAnimation.Pack(bank.Files.Single(f=>f.ID==1000000000+action.ID).Bytes.ToArray());
            float scale=p.Float(p.Object("hkaSplineCompressedAnimation")+16)/duration;
            foreach(var e in original.Events.Where(e=>e.Type is 0 or 1)) {
                var bytes=e.GetParameterBytes(false);Require(bytes.Length==12&&float.IsFinite(e.StartTime)&&float.IsFinite(e.EndTime)&&e.StartTime>=0&&e.EndTime>=e.StartTime&&e.EndTime<=600,"source event contract changed");
                if(e.Type==1)Require(BitConverter.ToInt32(bytes)==0&&BitConverter.ToInt32(bytes,4) is 0 or 1&&Judges.Contains(BitConverter.ToInt32(bytes,8)),"unsupported source contact or paired attack");
                else Require(BitConverter.ToInt32(bytes) is 7 or 23 or 78 or 79 or 86,"unsupported source action gate");
                var prototype=e.Type==1?hitPrototype:gatePrototype;var payload=new byte[16];bytes.CopyTo(payload,0);
                TAE.EventGroup? group=hitGroup;
                if(e.Type==0) {
                    int gateID=BitConverter.ToInt32(bytes);
                    if(!gateGroups.TryGetValue(gateID,out group)) {
                        group=gateGroups.Values.First()!.GetClone();action.EventGroups.Add(group);gateGroups.Add(gateID,group);
                    }
                }
                action.Events.Add(new(e.StartTime*scale,e.EndTime*scale,prototype.Type,prototype.Unk04,payload,false){Group=group});
            }
        }
        entry.Bytes=tae.Write();files["chr/"+target+".anibnd.dcx"]=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);return bake;
    }
    static int Offset(PARAMDEF def,string name) {
        int i=def.Fields.FindIndex(f=>f.InternalName==name);Require(i>=0,"missing native parameter field: "+name);
        var prefix=new PARAMDEF();prefix.Fields.AddRange(def.Fields.Take(i));return prefix.GetRowSize();
    }
    static void Copy(PARAM.Row source,byte[] bytes,PARAMDEF native,params string[] fields) {
        foreach(string name in fields) {
            var cell=source.Cells.Single(c=>c.Def.InternalName==name);var field=native.Fields.Single(f=>f.InternalName==name);
            Require(cell.Def.DisplayType==field.DisplayType&&cell.Def.BitSize==-1&&field.BitSize==-1&&field.ArrayLength==1,"parameter field ABI changed: "+name);
            byte[] value=field.DisplayType switch {
                PARAMDEF.DefType.f32=>BitConverter.GetBytes(Convert.ToSingle(cell.Value)),PARAMDEF.DefType.u16=>BitConverter.GetBytes(Convert.ToUInt16(cell.Value)),
                PARAMDEF.DefType.u32=>BitConverter.GetBytes(Convert.ToUInt32(cell.Value)),_=>throw new InvalidDataException("Unsupported foliage parameter field")};
            if(field.DisplayType==PARAMDEF.DefType.f32)Require(float.IsFinite(Convert.ToSingle(cell.Value)),"nonfinite source parameter");
            value.CopyTo(bytes,Offset(native,name));
        }
    }
    internal static byte[] Parameters(byte[] gameBytes,Func<string,byte[]?> source,string definitions,IReadOnlyList<(int Model,int SourceNpc)> bindings) {
        Require(bindings.Count is >0 and <=2&&bindings.Select(b=>b.Model).Distinct().Count()==bindings.Count&&bindings.All(b=>b is (8825,233000) or (8826,233001)),"invalid foliage owners");
        var game=BND4.Read(gameBytes);var original=BND3.Read(source("param/GameParam/GameParam.parambnd.dcx")!);
        BinderFile File(string name)=>game.Files.Single(f=>Leaf(f.Name)==name+".param");
        PARAMDEF Definition(string game,string name)=>PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../"+game+"/Defs/"+name+".xml")));
        PARAM Source(string name,string def) {var p=PARAM.Read(original.Files.Single(f=>Leaf(f.Name).Equals(name+".param",StringComparison.OrdinalIgnoreCase)).Bytes);p.ApplyParamdef(Definition("DS1R",def));return p;}
        var sourceNpc=Source("NpcParam","NpcParam");var sourceThink=Source("NpcThinkParam","NpcThinkParam");
        var npcFile=File("NpcParam");var npcs=MapCharacters.NativeRows(npcFile.Bytes,false);var npcDef=Definition("BB","NpcParam");
        var thinkFile=File("NpcThinkParam");var thinks=MapCharacters.NativeRows(thinkFile.Bytes,true);var thinkDef=Definition("BB","NpcThinkParam");
        var brain=MapCharacters.Payload(thinks.Rows.Single(r=>r.ID==205010));BitConverter.GetBytes(SourceEnemyAi.FoliageGoal).CopyTo(brain,4);
        Copy(sourceThink.Rows.Single(r=>r.ID==233000),brain,thinkDef,"nearDist","midDist","farDist","outDist","BackHomeLife_OnHitEneWal","eye_dist","ear_dist","ear_soundcut_dist","nose_dist","maxBackhomeDist","backhomeDist","backhomeBattleDist","BattleStartDist");
        ushort eye=Convert.ToUInt16(sourceThink.Rows.Single(r=>r.ID==233000)["eye_dist"].Value),battle=Convert.ToUInt16(sourceThink.Rows.Single(r=>r.ID==233000)["BattleStartDist"].Value);
        foreach(string field in new[]{"eyeDist_forDark","eyeDist_forPitchDark"})BitConverter.GetBytes(eye).CopyTo(brain,Offset(thinkDef,field));
        foreach(string field in new[]{"battleStartDist_forDark","battleStartDist_forPitchDark"})BitConverter.GetBytes(battle).CopyTo(brain,Offset(thinkDef,field));
        MapCharacters.CloneRow(thinks,thinks.Rows.Single(r=>r.ID==205010),SourceEnemyAi.FoliageGoal,"Imported source foliage AI",brain);
        var behaviorFile=File("BehaviorParam");var behavior=ImportParam.Read(behaviorFile.Bytes,"BEHAVIOR_PARAM_ST",32,2);
        var attackFile=File("AtkParam_Npc");var attacks=ImportParam.Read(attackFile.Bytes,"ATK_PARAM_ST",164,1);
        var rawSource=PARAM.Read(original.Files.Single(f=>Leaf(f.Name)=="AtkParam_Npc.param").Bytes);Require(rawSource.ParamType=="ATK_PARAM_ST"&&rawSource.DetectedSize==128&&!rawSource.BigEndian,"source attack row ABI changed");
        var rawDef=new PARAMDEF{ParamType=rawSource.ParamType,DataVersion=rawSource.ParamdefDataVersion};rawDef.Fields.Add(new PARAMDEF.Field(rawDef,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=128});rawSource.ApplyParamdef(rawDef);
        var sourceDef=Definition("DS1R","AtkParam");var nativeDef=Definition("BB","AtkParam");
        Require(sourceDef.Fields.Count==71&&sourceDef.GetRowSize()==128&&sourceDef.Fields.Take(70).Zip(nativeDef.Fields.Take(70)).All(p=>p.First.InternalName==p.Second.InternalName&&p.First.DisplayType==p.Second.DisplayType&&p.First.ArrayLength==p.Second.ArrayLength&&p.First.BitSize==p.Second.BitSize)&&Offset(sourceDef,"pad")==127&&Offset(nativeDef,"atkPow_forSfxSe")==127,"source/native attack prefix changed");
        var sourceBehavior=Source("BehaviorParam","BehaviorParam");var sourceAttacks=Source("AtkParam_Npc","AtkParam");
        foreach(var binding in bindings) {
            int variation=binding.Model*10;var row=npcs.Rows.Single(r=>r.ID==binding.Model*100);var payload=MapCharacters.Payload(row);
            Require(BitConverter.ToInt32(payload)==20500,"foliage NPC is not an owned donor clone");
            BitConverter.GetBytes(variation).CopyTo(payload,0);BitConverter.GetBytes(SourceEnemyAi.FoliageGoal).CopyTo(payload,4);
            Copy(sourceNpc.Rows.Single(r=>r.ID==binding.SourceNpc),payload,npcDef,"hp");BitConverter.GetBytes((short)-1).CopyTo(payload,Offset(npcDef,"normalChangeAnimChrId"));row.Cells.Single().Value=payload;
            foreach(int judge in Judges) {
                var from=sourceBehavior.Rows.Single(r=>Convert.ToInt32(r["variationId"].Value)==23300&&Convert.ToInt32(r["behaviorJudgeId"].Value)==judge);
                Require(Convert.ToInt32(from["refType"].Value)==0,"source contact is not melee");int sourceID=Convert.ToInt32(from["refId"].Value);
                // Source3006 has two simultaneous hands; retail c2050 only
                // supplies judge160. Clone that verified melee row layout for
                // owned judge161, with its own source attack and hit index.
                int donorJudge=judge==161?160:judge;var donor=behavior.Rows.Single(r=>r.ID==220500000+donorJudge);var data=ImportParam.Payload(donor);
                Require(BitConverter.ToInt32(data)==20500&&BitConverter.ToInt32(data,4)==donorJudge&&data[8]==2&&data[9]==0,"native melee behavior ABI changed");
                int attack=binding.Model*100+sourceID%100;BitConverter.GetBytes(variation).CopyTo(data,0);BitConverter.GetBytes(judge).CopyTo(data,4);BitConverter.GetBytes(attack).CopyTo(data,12);
                ImportParam.Append(behavior,donor,200000000+variation*1000+judge,"Imported foliage contact "+binding.Model+"/"+judge,data);
                var target=attacks.Rows.Single(r=>r.ID==205002);var damage=ImportParam.Payload(target);var authored=ImportParam.Payload(rawSource.Rows.Single(r=>r.ID==sourceID));
                var attackSource=sourceAttacks.Rows.Single(r=>r.ID==sourceID);Require(Convert.ToInt32(attackSource["throwFlag"].Value)==0&&Convert.ToInt32(attackSource["throwTypeId"].Value)==0,"source paired throw cannot become ordinary damage");authored.AsSpan(0,127).CopyTo(damage);
                // DS1's physical Push attribute4 is BB's Non-Attribute4.
                // Use ordinary native Physical1; these are wooden claw blows,
                // with native Blunt1/Wood2 contact effects rather than a numeric
                // cross-game material or resistance-class substitution.
                Require(Convert.ToInt32(attackSource["atkAttribute"].Value)==4&&Convert.ToInt32(attackSource["spAttribute"].Value)==1&&Convert.ToInt32(attackSource["atkType"].Value)==1,"unverified source damage classification");
                damage[Offset(nativeDef,"atkAttribute")]=1;damage[Offset(nativeDef,"atkType")]=1;damage[Offset(nativeDef,"atkMaterial")]=2;
                for(int i=0;i<5;i++){Require(BitConverter.ToInt32(damage,24+i*4)==0,"source attack needs an unported effect");BitConverter.GetBytes(-1).CopyTo(damage,24+i*4);}
                for(int i=0;i<4;i++)foreach(int offset in new[]{44+i*2,52+i*2}) {
                    short dummy=BitConverter.ToInt16(damage,offset);bool active=BitConverter.ToSingle(damage,i*4)>0;
                    Require(!active||dummy is 1 or 2 or 10 or 11 or 12 or 20 or 21 or 22 or -1,"unverified foliage attack socket");
                    BitConverter.GetBytes(active&&dummy>=0?checked((short)(dummy+ContactBase)):(short)-1).CopyTo(damage,offset);
                }
                ImportParam.Append(attacks,target,attack,"Imported foliage attack "+binding.Model+"/"+sourceID,damage);
            }
        }
        npcFile.Bytes=MapCharacters.WriteRows(npcs);thinkFile.Bytes=MapCharacters.WriteRows(thinks);behaviorFile.Bytes=ImportParam.Write(behavior);attackFile.Bytes=ImportParam.Write(attacks);
        return game.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
    internal static readonly string[] Limitations=["Twelve ordinary Darkroot foliage use source attack selection, spacing, interruption helpers, body motion, four claw attacks, contact sockets and base damage. Source grab3004 is explicitly redirected to left claw3003 until paired attacker/player playback is converted. Fourteen hidden wake actors and the disabled Butterfly helper remain omitted.","Native locomotion speed, action duration, generic logic, physical/ragdoll anchors and sound/effects remain approximations. Source combat windows follow the imported pose at its normalized native timing."];
}

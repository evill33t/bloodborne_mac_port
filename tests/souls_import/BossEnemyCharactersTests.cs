// SPDX-License-Identifier: GPL-3.0-or-later
#nullable enable
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;
static class BossEnemyCharactersTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Reject(Action action,string message){try{action();}catch(InvalidDataException){return;}throw new Exception(message);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    // Canonical node-name order lets a byte comparison cover every serialized
    // render record while treating indices as references to named nodes.
    static void Permute(FLVER2 model,IEnumerable<string> names) {
        var original=model.Nodes.ToArray();var byName=names.Select((name,index)=>(name,index)).ToDictionary(x=>x.name,x=>x.index);int Remap(int i)=>i<0?i:byName[original[i].Name];short Link(short i)=>checked((short)Remap(i));
        foreach(var n in original){n.ParentIndex=Link(n.ParentIndex);n.FirstChildIndex=Link(n.FirstChildIndex);n.NextSiblingIndex=Link(n.NextSiblingIndex);n.PreviousSiblingIndex=Link(n.PreviousSiblingIndex);}
        model.Nodes=original.OrderBy(n=>byName[n.Name]).ToList();foreach(var d in model.Dummies){d.ParentBoneIndex=Link(d.ParentBoneIndex);d.AttachBoneIndex=Link(d.AttachBoneIndex);}
        foreach(var m in model.Meshes){m.NodeIndex=Remap(m.NodeIndex);if(m.BoneIndices.Count>0)m.BoneIndices=m.BoneIndices.Select(Remap).ToList();else foreach(var v in m.Vertices){if(m.UseBoneWeights)for(int k=0;k<4;k++)v.BoneIndices[k]=Remap(v.BoneIndices[k]);else v.NormalW=Remap(v.NormalW);}}
    }
    static byte[] Canonical(FLVER2 model){var copy=FLVER2.Read(model.Write());Permute(copy,copy.Nodes.Select(n=>n.Name).Order(StringComparer.Ordinal));return copy.Write();}
    static void CheckRenderOrder(FLVER2 model) {
        int active=model.Nodes.Count(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0);
        Check(model.Nodes.Take(active).All(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)==0)&&model.Nodes.Skip(active).All(n=>(n.Flags&FLVER.Node.NodeFlags.Disabled)!=0),"Native active-node prefix/disabled-tail contract was lost");
        foreach(var mesh in model.Meshes)foreach(var v in mesh.Vertices)for(int k=0;k<4;k++)if(v.BoneWeights[k]>0)Check(SkinRetarget.Bone(mesh,v.BoneIndices[k])<active,"Skin exceeds the native active render palette");
    }
    // Inspect serialized fixups independently of NativeAnimation.Pack before
    // decoding poses. hkStringPtr's low bit is ownership, not string data.
    sealed class RawStrings {
        readonly byte[] bytes;readonly Dictionary<int,int> pointers=new();readonly Dictionary<string,int> objects=new();
        internal readonly int Data;
        internal RawStrings(byte[] bytes) {
            this.bytes=bytes;int section=64+BitConverter.ToUInt16(bytes,62),classes=BitConverter.ToInt32(bytes,section+20),header=section+128;
            Data=BitConverter.ToInt32(bytes,header+20);int local=BitConverter.ToInt32(bytes,header+24),global=BitConverter.ToInt32(bytes,header+28),virtuals=BitConverter.ToInt32(bytes,header+32),end=BitConverter.ToInt32(bytes,header+36);
            for(int at=local;at+8<=global;at+=8)if(Int(at)!=-1)pointers.Add(Int(at),Int(at+4));
            for(int at=virtuals;at+12<=end;at+=12)if(Int(at)!=-1){int text=classes+Int(at+8),stop=Array.IndexOf(bytes,(byte)0,text);objects.Add(System.Text.Encoding.ASCII.GetString(bytes,text,stop-text),Int(at));}
        }
        internal int Int(int at)=>BitConverter.ToInt32(bytes,Data+at);
        internal int Object(string name)=>objects[name];
        internal int Pointer(int field)=>pointers[field];
        internal byte[] Text(int field) {
            int at=Pointer(field);Check(((Data+at)&1)==0,"Borrowed hkStringPtr has heap-owned low bit at "+field);
            Check(BitConverter.ToUInt64(bytes,Data+field)==0,"Serialized borrowed string pointer is not a zero fixup placeholder");
            int stop=Array.IndexOf(bytes,(byte)0,Data+at);return bytes.AsSpan(Data+at,stop-Data-at).ToArray();
        }
        internal int[] Bones(){int obj=Object("hkaSkeleton"),at=Pointer(obj+40);return Enumerable.Range(0,Int(obj+48)).Select(i=>at+i*16).ToArray();}
    }
    static Matrix4x4 Sample(Spline.TransformTrack t,float time) {
        Vector3 p=t.StaticPosition,s=t.StaticScale;
        if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(time)??p.X,t.SplinePosition.GetValueY(time)??p.Y,t.SplinePosition.GetValueZ(time)??p.Z);
        if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(time)??s.X,t.SplineScale.GetValueY(time)??s.Y,t.SplineScale.GetValueZ(time)??s.Z);
        var q=t.HasSplineRotation?t.SplineRotation.GetValue(time):t.StaticRotation;
        return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(p);
    }
    static float Error(Matrix4x4 a,Matrix4x4 b) {
        float[] x=[a.M11,a.M12,a.M13,a.M21,a.M22,a.M23,a.M31,a.M32,a.M33,a.M41,a.M42,a.M43];float[] y=[b.M11,b.M12,b.M13,b.M21,b.M22,b.M23,b.M31,b.M32,b.M33,b.M41,b.M42,b.M43];return x.Zip(y,(x,y)=>Math.Abs(x-y)).Max();
    }
    static void AiClosure(Func<string,byte[]> read,string definitions,string output) {
        var parameters=BND4.Read(read("param/gameparam/gameparam.parambnd.dcx"));var think=PARAM.Read(parameters.Files.Single(f=>Leaf(f.Name)=="NpcThinkParam.param").Bytes);think.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs/NpcThinkParam.xml"))));
        Check(new[]{BossEnemyCharacters.Taurus,BossEnemyCharacters.GargoylePrimary,BossEnemyCharacters.GargoyleSecondary}.All(p=>p.Think==313090&&p.Npc==313000)&&NonHumanoidEnemies.Demon.Think==313000,"Boss engagement profile changed the NPC donor or the ordinary Titanite adapter");
        var generic=think.Rows.Single(r=>r.ID==313000);var boss=think.Rows.Single(r=>r.ID==313090);
        int Value(PARAM.Row row,string field)=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName==field).Value);
        Check(Value(generic,"logicId")==Value(boss,"logicId")&&Value(boss,"logicId")==10000&&Value(generic,"battleGoalID")==Value(boss,"battleGoalID")&&Value(boss,"battleGoalID")==313000,"Native sword boss does not share the exact generic logic/battle scripts");
        Check(Value(generic,"BattleStartDist")==10&&Value(boss,"BattleStartDist")==100&&Value(generic,"nose_dist")==0&&Value(boss,"nose_dist")==100,"Installed boss engagement distances changed");
        var retailBoss=MSBB.Read(read("map/MapStudio/m29_21_00_00/m29_21_00_02.msb.dcx"));
        Check(retailBoss.Parts.Enemies.Any(p=>p.ModelName=="c3130"&&p.ThinkParamID==313090),"Installed retail sword boss no longer uses the selected native Think row");
        byte[] target=read("script/m24_00_00_00.luabnd.dcx"),shared=read("script/aicommon.luabnd.dcx");var initial=BND4.Read(target);var initialInfo=LUAINFO.Read(initial.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)).Bytes);
        Check(!initialInfo.Goals.Any(g=>g.ID==10000)&&!initial.Files.Any(f=>Leaf(f.Name)=="010000_logic.lua"),"Installed destination no longer reproduces missing shared logic dependency");
        var common=BND4.Read(shared);var commonGoal=LUAINFO.Read(common.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)).Bytes).Goals.Single(g=>g.ID==10000);var commonLogic=common.Files.Single(f=>Leaf(f.Name)=="010000_logic.lua");var reports=new List<object>();
        target=MapNativeAi.Merge(target,shared,requiredGoals:[10000]);var seeded=BND4.Read(target);foreach(var f in initial.Files.Where(f=>!Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)&&!Leaf(f.Name).EndsWith(".luagnl",StringComparison.Ordinal)))Check(seeded.Files.Single(t=>Leaf(t.Name)==Leaf(f.Name)).Bytes.Span.SequenceEqual(f.Bytes.Span),"Shared AI premerge changed an existing script");
        foreach(var profile in new[]{NonHumanoidEnemies.Boar,NonHumanoidEnemies.Demon,BossEnemyCharacters.Taurus,BossEnemyCharacters.Capra,BossEnemyCharacters.Dog,BossEnemyCharacters.GargoylePrimary,BossEnemyCharacters.GargoyleSecondary}) {
            var row=think.Rows.Single(r=>r.ID==profile.Think);int logic=Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="logicId").Value),battle=Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="battleGoalID").Value);Check(logic==10000,"Native nonhumanoid shared logic contract changed");int model=int.Parse(profile.Native[1..]);byte[] donor=read("script/"+profile.Ai+".luabnd.dcx"),before=target.ToArray();
            target=MapNativeAi.Merge(target,donor,model);var after=BND4.Read(target);var info=LUAINFO.Read(after.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)).Bytes);var copied=info.Goals.Single(g=>g.ID==logic);
            Check(copied.Name==commonGoal.Name&&copied.BattleInterrupt==commonGoal.BattleInterrupt&&copied.LogicInterrupt==commonGoal.LogicInterrupt&&copied.LogicInterruptName==commonGoal.LogicInterruptName,"Shared logic registry contract changed");Check(after.Files.Single(f=>Leaf(f.Name)=="010000_logic.lua").Bytes.Span.SequenceEqual(commonLogic.Bytes.Span),"Shared native logic bytecode changed");Check(info.Goals.Any(g=>g.ID==battle),"Native battle goal is missing after logic closure");
            var old=BND4.Read(before);foreach(var f in old.Files.Where(f=>!Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)&&!Leaf(f.Name).EndsWith(".luagnl",StringComparison.Ordinal)))Check(after.Files.Single(t=>Leaf(t.Name)==Leaf(f.Name)).Bytes.Span.SequenceEqual(f.Bytes.Span),"AI dependency merge changed an existing script");var oldInfo=LUAINFO.Read(old.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)).Bytes);foreach(var g in oldInfo.Goals)Check(info.Goals.Any(t=>t.ID==g.ID&&t.Name==g.Name&&t.BattleInterrupt==g.BattleInterrupt&&t.LogicInterrupt==g.LogicInterrupt&&t.LogicInterruptName==g.LogicInterruptName),"AI dependency merge changed an existing goal");
            Check(MapNativeAi.Merge(target,shared,requiredGoals:[logic]).AsSpan().SequenceEqual(target),"Repeated shared dependency duplicated or changed native records");reports.Add(new{profile.Source,profile.Native,profile.Think,logic,battle,goal=copied.Name});
        }
        var missingFile=BND4.Read(shared);missingFile.Files.RemoveAll(f=>Leaf(f.Name)=="010000_logic.lua");Reject(()=>MapNativeAi.Merge(read("script/m24_00_00_00.luabnd.dcx"),missingFile.Write(DCX.Type.DCX_DFLT_10000_44_9),3130,requiredGoals:[10000]),"Missing required logic bytecode was accepted");
        var missingGoal=BND4.Read(shared);var infoFile=missingGoal.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal));var sourceInfo=LUAINFO.Read(infoFile.Bytes);sourceInfo.Goals.RemoveAll(g=>g.ID==10000);infoFile.Bytes=sourceInfo.Write();Reject(()=>MapNativeAi.Merge(read("script/m24_00_00_00.luabnd.dcx"),missingGoal.Write(DCX.Type.DCX_DFLT_10000_44_9),3130,requiredGoals:[10000]),"Missing required logic registry was accepted");
        var conflicting=BND4.Read(target);var bad=conflicting.Files.Single(f=>Leaf(f.Name)=="010000_logic.lua");var badBytes=bad.Bytes.ToArray();badBytes[^1]^=1;bad.Bytes=badBytes;Reject(()=>MapNativeAi.Merge(conflicting.Write(DCX.Type.DCX_DFLT_10000_44_9),shared,3130,requiredGoals:[10000]),"Conflicting native shared bytecode was accepted");
        conflicting=BND4.Read(target);infoFile=conflicting.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal));sourceInfo=LUAINFO.Read(infoFile.Bytes);sourceInfo.Goals.Single(g=>g.ID==10000).Name="Conflicting_logic";infoFile.Bytes=sourceInfo.Write();Reject(()=>MapNativeAi.Merge(conflicting.Write(DCX.Type.DCX_DFLT_10000_44_9),shared,3130,requiredGoals:[10000]),"Conflicting native shared goal identity was accepted");
        Reject(()=>MapNativeAi.Merge(target,shared,3130,requiredGoals:[10000,10000]),"Duplicate dependency identity was accepted");Directory.CreateDirectory(output);File.WriteAllText(Path.Combine(output,"ai-validation.json"),JsonSerializer.Serialize(new{profiles=reports,nativeBossThink=313090,sharedBattleGoal=313000,missingLogicReproduced=true,requiredGoal=10000,bytecodeSha=Convert.ToHexString(SHA256.HashData(commonLogic.Bytes.Span)),guards=5},new JsonSerializerOptions{WriteIndented=true}));Console.WriteLine("PASS native AI closure: seven profiles / five native Think rows, retail boss313090 shares exact logic10000/battle313000, preserved existing scripts/goals, five rejection guards");
    }
    static void Health(byte[] original,PARAM sourceRows,string definitions,string output) {
        var game=BND4.Read(original);var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var effectFile=game.Files.Single(f=>Leaf(f.Name)=="SpEffectParam.param");
        var npcs=ImportParam.Read(npcFile.Bytes,"NPC_PARAM_ST",388,5);var effects=ImportParam.Read(effectFile.Bytes,"SP_EFFECT_PARAM_ST",528,3);
        (int ID,int Donor,int Source,int Observed)[] actors=[(895000,313000,225000,5145),(895100,273100,224000,1728),(895200,114000,334003,182),(895300,313000,535000,3527),(895400,313000,535001,3527)];
        (int Source,int Target)[] remaps=[(7008,895710),(7013,895711),(7023,895712),(7405,895713),(7408,895714),(7426,895715)];
        foreach(var a in actors){var donor=npcs.Rows.Single(r=>r.ID==a.Donor);var source=sourceRows.Rows.Single(r=>r.ID==a.Source);ImportParam.Append(npcs,donor,a.ID,"Imported boss encounter actor health fixture",BossEnemyCharacters.Parameters(ImportParam.Payload(donor),source,definitions));}
        // Exercise null-offset preservation even if a different retail dump
        // happens to have names on these rows.
        npcs.Rows[0].Name=null;effects.Rows[0].Name=null;
        npcFile.Bytes=ImportParam.Write(npcs);effectFile.Bytes=ImportParam.Write(effects);byte[] input=game.Write(DCX.Type.DCX_DFLT_10000_44_9),inputHash=SHA256.HashData(input);
        byte[] normalized=BossEnemyCharacters.NormalizeHealth(input,definitions);var after=BND4.Read(normalized);
        var oldNpcs=ImportParam.Read(npcFile.Bytes,"NPC_PARAM_ST",388,5);var oldEffects=ImportParam.Read(effectFile.Bytes,"SP_EFFECT_PARAM_ST",528,3);
        var newNpcs=ImportParam.Read(after.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,"NPC_PARAM_ST",388,5);var newEffects=ImportParam.Read(after.Files.Single(f=>Leaf(f.Name)=="SpEffectParam.param").Bytes,"SP_EFFECT_PARAM_ST",528,3);
        Check(oldNpcs.Rows.Select(r=>r.ID).SequenceEqual(newNpcs.Rows.Select(r=>r.ID)),"HP adaptation changed NPC row order or count");
        Check(oldEffects.Rows.Select(r=>r.ID).SequenceEqual(newEffects.Rows.Where(r=>!remaps.Any(m=>m.Target==r.ID)).Select(r=>r.ID)),"HP adaptation changed retail effect row order");
        foreach(var row in oldEffects.Rows){var other=newEffects.Rows.Single(r=>r.ID==row.ID);Check(row.Name==other.Name&&ImportParam.Payload(row).SequenceEqual(ImportParam.Payload(other)),"HP adaptation changed an original native effect");}
        foreach(var row in oldNpcs.Rows) {
            var other=newNpcs.Rows.Single(r=>r.ID==row.ID);Check(row.Name==other.Name,"HP adaptation normalized a null NPC name");
            if(!actors.Any(a=>a.ID==row.ID)){Check(ImportParam.Payload(row).SequenceEqual(ImportParam.Payload(other)),"HP adaptation changed an unrelated NPC");continue;}
            byte[] expected=ImportParam.Payload(row);for(int at=72;at<=104;at+=4){int id=BitConverter.ToInt32(expected,at);foreach(var m in remaps)if(id==m.Source)BitConverter.GetBytes(m.Target).CopyTo(expected,at);}
            Check(expected.SequenceEqual(ImportParam.Payload(other)),"HP adaptation changed non-effect-reference owned NPC bytes");
        }
        Check(newEffects.Rows.Count==oldEffects.Rows.Count+6,"HP adaptation did not create precisely six owned effects");
        foreach(var m in remaps){byte[] expected=ImportParam.Payload(oldEffects.Rows.Single(r=>r.ID==m.Source));BitConverter.GetBytes(1f).CopyTo(expected,16);Check(expected.SequenceEqual(ImportParam.Payload(newEffects.Rows.Single(r=>r.ID==m.Target))),"HP-neutral effect changed damage, defense or other donor bytes");}
        // Use the actual typed retail schema, independently of the adapter's
        // raw offset, to verify maxHpRate and every retained combat scalar.
        var typed=PARAM.Read(after.Files.Single(f=>Leaf(f.Name)=="SpEffectParam.param").Bytes);typed.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs/SpEffect.xml"))));
        foreach(var m in remaps){var donor=typed.Rows.Single(r=>r.ID==m.Source);var clone=typed.Rows.Single(r=>r.ID==m.Target);for(int i=0;i<donor.Cells.Count;i++)Check(clone.Cells[i].Def.InternalName==donor.Cells[i].Def.InternalName&&(donor.Cells[i].Def.InternalName=="maxHpRate"?Convert.ToSingle(clone.Cells[i].Value)==1:Equals(donor.Cells[i].Value,clone.Cells[i].Value)||donor.Cells[i].Value is byte[] x&&clone.Cells[i].Value is byte[] y&&x.SequenceEqual(y)),"HP-neutral effect changed a typed non-HP field");}
        var health=new List<object>();foreach(var a in actors) {
            byte[] before=ImportParam.Payload(oldNpcs.Rows.Single(r=>r.ID==a.ID)),current=ImportParam.Payload(newNpcs.Rows.Single(r=>r.ID==a.ID));uint hp=BitConverter.ToUInt32(current,32);float previousRate=BitConverter.ToSingle(ImportParam.Payload(oldEffects.Rows.Single(r=>r.ID==BitConverter.ToInt32(before,96))),16);
            Check((int)(hp*previousRate)==a.Observed,"Actual resident effect no longer explains observed native HP");
            foreach(int offset in new[]{96,104}){var e=typed.Rows.Single(r=>r.ID==BitConverter.ToInt32(current,offset));Check(Convert.ToSingle(e.Cells.Single(c=>c.Def.InternalName=="maxHpRate").Value)==1,"NG or NG+ HP still scales");}
            health.Add(new{a.ID,sourceHP=hp,observedBefore=a.Observed,previousRate,expectedAfter=hp});
        }
        foreach(var file in game.Files){var other=after.Files.Single(f=>f.ID==file.ID);Check(file.Name==other.Name&&file.Flags==other.Flags,"HP adaptation changed binder member identity");if(Leaf(file.Name) is not("NpcParam.param" or "SpEffectParam.param"))Check(file.Bytes.Span.SequenceEqual(other.Bytes.Span),"HP adaptation changed an unrelated parameter table");}
        Check(SHA256.HashData(input).SequenceEqual(inputHash),"HP adaptation mutated input bytes");
        Reject(()=>BossEnemyCharacters.NormalizeHealth(original,definitions),"Missing imported encounter cohort was accepted");
        Reject(()=>BossEnemyCharacters.NormalizeHealth(normalized,definitions),"Duplicate HP-effect ownership was accepted");
        var conflict=BND4.Read(input);var conflictFile=conflict.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var conflictRows=ImportParam.Read(conflictFile.Bytes,"NPC_PARAM_ST",388,5);var conflictData=ImportParam.Payload(conflictRows.Rows[0]);BitConverter.GetBytes(895710).CopyTo(conflictData,0);conflictRows.Rows[0].Cells.Single().Value=conflictData;conflictFile.Bytes=ImportParam.Write(conflictRows);
        Reject(()=>BossEnemyCharacters.NormalizeHealth(conflict.Write(DCX.Type.DCX_DFLT_10000_44_9),definitions),"Forward reference into owned effect reservation was accepted");
        var unsupported=BND4.Read(input);var unsupportedFile=unsupported.Files.Single(f=>Leaf(f.Name)=="SpEffectParam.param");var unsupportedRows=ImportParam.Read(unsupportedFile.Bytes,"SP_EFFECT_PARAM_ST",528,3);var unsupportedRow=unsupportedRows.Rows.Single(r=>r.ID==7013);var unsupportedData=ImportParam.Payload(unsupportedRow);BitConverter.GetBytes(float.NaN).CopyTo(unsupportedData,16);unsupportedRow.Cells.Single().Value=unsupportedData;unsupportedFile.Bytes=ImportParam.Write(unsupportedRows);
        Reject(()=>BossEnemyCharacters.NormalizeHealth(unsupported.Write(DCX.Type.DCX_DFLT_10000_44_9),definitions),"Unvalidated native scaling effect was accepted");
        Directory.CreateDirectory(output);File.WriteAllText(Path.Combine(output,"health-validation.json"),JsonSerializer.Serialize(new{actors=health,mappings=remaps.Select(m=>new{m.Source,m.Target}),originalNpcRows=oldNpcs.Rows.Count-actors.Length,fixtureNpcRows=oldNpcs.Rows.Count,originalEffectRows=oldEffects.Rows.Count,clones=6,guards=4,inputHash=Convert.ToHexString(inputHash)},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS boss HP: five owners, six HP-only effects, {oldNpcs.Rows.Count-actors.Length} native NPC rows / {oldEffects.Rows.Count} native effects preserved; four rejection guards");
    }
    public static void Main(string[] args) {
        string source=args[0],native=args[1],defs=args[2];var originals=new Dictionary<string,byte[]>();
        byte[] ReadFile(string p){var bytes=File.ReadAllBytes(p);originals.TryAdd(p,SHA256.HashData(bytes));return bytes;}
        byte[]? Source(string p)=>File.Exists(Path.Combine(source,p))?ReadFile(Path.Combine(source,p)):null;
        byte[] Native(string p)=>ReadFile(Path.Combine(native,p));
        Check(SourceAnimation.DecodeBoneName("Master"u8.ToArray())=="Master","ASCII bone names changed");
        byte[] japanese=Convert.FromHexString("6D6173746572208347834E83588367838930314E7562");
        Check(SourceAnimation.DecodeBoneName(japanese)=="master エクストラ01Nub","Authored Shift-JIS bone name did not match FLVER");
        Check(SourceAnimation.DecodeBoneName(System.Text.Encoding.UTF8.GetBytes("master エクストラ01Nub"))=="master エクストラ01Nub","UTF8 source bone names changed");
        Reject(()=>SourceAnimation.DecodeBoneName([65,0,66]),"Embedded NUL bone name accepted");
        bool malformed=false;try{SourceAnimation.DecodeBoneName([0x81]);}catch(System.Text.DecoderFallbackException){malformed=true;}Check(malformed,"Malformed legacy bone name accepted");
        var sourceMap=MSB1.Read(Source("map/MapStudio/m10_01_00_00.msb")!);var sourceGame=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx")!);var sourceRows=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);sourceRows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"NpcParam.xml")));
        var selected=sourceMap.Parts.Enemies.Where(p=>BossEnemyCharacters.Select(p,"m10_01_00_00")!=null).ToArray();
        Check(selected.Select(p=>p.EntityID).Order().SequenceEqual(new[]{1010500,1010501,1010700,1010750,1010800,1010801}),"Encounter selection lost bosses/dogs or included source-disabled duplicate dogs");
        Check(selected.Count(p=>BossEnemyCharacters.IsBoss(BossEnemyCharacters.Select(p,"m10_01_00_00")!))==4,"Auxiliary dogs were counted as bosses");
        Check(selected.All(p=>BossEnemyCharacters.Select(p,"m10_02_00_00")==null),"Boss profile escaped its authored map");
        foreach(var actor in selected){var invalid=(MSB1.Part.Enemy)actor.DeepCopy();invalid.NPCParamID++;Check(BossEnemyCharacters.Select(invalid,"m10_01_00_00")==null,"Unvalidated boss parameter variant accepted");invalid=(MSB1.Part.Enemy)actor.DeepCopy();invalid.InitAnimID=9000;Check(BossEnemyCharacters.Select(invalid,"m10_01_00_00")==null,"Unvalidated source initial animation accepted");}
        var nativeGame=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));var nativeRows=MapCharacters.NativeRows(nativeGame.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);var thinkRows=PARAM.Read(nativeGame.Files.Single(f=>Leaf(f.Name)=="NpcThinkParam.param").Bytes);thinkRows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(defs,"../../BB/Defs/NpcThinkParam.xml"))));
        bool aiOnly=args.Length>5&&args[5]=="1";
        if(args.Length<=4||args[4]!="1")AiClosure(Native,defs,args[3]);
        if(aiOnly){foreach(var(path,hash)in originals)Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash),"Original asset changed: "+path);return;}
        Health(Native("param/gameparam/gameparam.parambnd.dcx"),sourceRows,defs,args[3]);
        if(args.Length>4&&args[4]=="1"){foreach(var(path,hash)in originals)Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash),"Original asset changed: "+path);return;}
        int checkedClips=0,checkedPoses=0;float maxPoseError=0;var result=new List<object>();
        foreach(var profile in new[]{BossEnemyCharacters.Taurus,BossEnemyCharacters.Capra,BossEnemyCharacters.Dog,BossEnemyCharacters.GargoylePrimary,BossEnemyCharacters.GargoyleSecondary}) {
            var actor=selected.First(p=>BossEnemyCharacters.Select(p,"m10_01_00_00")==profile);var row=sourceRows.Rows.Single(r=>r.ID==actor.NPCParamID);
            var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
            string target=profile==BossEnemyCharacters.Taurus?"c8950":profile==BossEnemyCharacters.Capra?"c8951":profile==BossEnemyCharacters.Dog?"c8952":profile==BossEnemyCharacters.GargoylePrimary?"c8953":"c8954";var files=new Dictionary<string,byte[]>();
            if(BossEnemyCharacters.IsGargoyle(profile))Check(masks.SetEquals(profile==BossEnemyCharacters.GargoylePrimary?new[]{0,1}:new[]{2}),"Gargoyle source equipment variant masks changed");
            var skin=BossEnemyCharacters.Package(Source,Native,profile,target,masks,files);
            if(BossEnemyCharacters.IsGargoyle(profile)){Check(skin.Model.Nodes.Count==248,"Gargoyle combined rig exceeds verified render palette contract");Check(skin.Model.Materials.Any(m=>m.MTD.EndsWith("_e.mtd",StringComparison.Ordinal)),"Gargoyle torn wings/edge materials lost alpha testing");}
            Check(skin.DirectWeightShare>=.999999&&skin.ProportionScale==1,"Boss relies on guessed anatomy or actor scale");CheckRenderOrder(FLVER2.Read(skin.Model.Write()));
            var authored=MapCharacters.Authored(Source,profile.Source);var original=FLVER2.Read(authored.Model);var sourceMeshes=original.Meshes.Where(m=>{var match=System.Text.RegularExpressions.Regex.Match(original.Materials[m.MaterialIndex].Name,@"#(\d+)#");return !match.Success||masks.Contains(int.Parse(match.Groups[1].Value));}).ToArray();
            Check(sourceMeshes.Length==skin.Model.Meshes.Count,"Boss mesh/equipment selection changed");float maxVertexError=0;
            for(int mesh=0;mesh<sourceMeshes.Length;mesh++){Check(sourceMeshes[mesh].Vertices.Count==skin.Model.Meshes[mesh].Vertices.Count,"Source vertices lost");for(int v=0;v<sourceMeshes[mesh].Vertices.Count;v++)maxVertexError=Math.Max(maxVertexError,Vector3.Distance(sourceMeshes[mesh].Vertices[v].Position,skin.Model.Meshes[mesh].Vertices[v].Position));}
            Check(maxVertexError<.0001f,"Boss source geometry was stretched: "+maxVertexError);
            var before=BND4.Read(Native("chr/"+profile.Native+".anibnd.dcx"));var after=BND4.Read(files["chr/"+target+".anibnd.dcx"]);Check(before.Files.Count==after.Files.Count,"Native playback binder members changed");
            var rawOld=new RawStrings(before.Files.Single(f=>f.ID==1000000).Bytes.ToArray());var rawNew=new RawStrings(after.Files.Single(f=>f.ID==1000000).Bytes.ToArray());var oldBoneFields=rawOld.Bones();var newBoneFields=rawNew.Bones();
            for(int i=0;i<oldBoneFields.Length;i++)Check(rawOld.Pointer(oldBoneFields[i])==rawNew.Pointer(newBoneFields[i])&&rawOld.Text(oldBoneFields[i]).SequenceEqual(rawNew.Text(newBoneFields[i])),"Native borrowed bone name/target changed");
            var idle=SourceAnimation.Load(Source,profile.Source,0);for(int i=0;i<idle.Bones.Count;i++)Check(rawNew.Text(newBoneFields[oldBoneFields.Length+i]).SequenceEqual(System.Text.Encoding.UTF8.GetBytes("DS1_"+idle.Bones[i].Name)),"Source borrowed name lost or has invalid native alignment");
            var rig=NativeAnimation.Skeleton(after.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),allowMirroredScale:true);var oldRig=NativeAnimation.Skeleton(before.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
            Check(rig.Names.Length==profile.NativeBones+idle.Bones.Count&&rig.Names.Take(oldRig.Names.Length).SequenceEqual(oldRig.Names)&&rig.Local.Take(oldRig.Local.Length).SequenceEqual(oldRig.Local),"Native skeleton bones/reference transforms changed");
            Check(rig.Names.Skip(oldRig.Names.Length).SequenceEqual(idle.Bones.Select(b=>"DS1_"+b.Name)),"Source skeleton order changed");Reject(()=>BossEnemyCharacters.SourceSlot(profile,9999),"Unmapped native boss action was silently accepted");
            var cache=new Dictionary<int,SourceAnimation>();
            foreach(var old in before.Files.Where(f=>f.ID>=1000000000)) {
                var converted=after.Files.Single(f=>f.ID==old.ID).Bytes.ToArray();var oldStrings=new RawStrings(old.Bytes.ToArray());var newStrings=new RawStrings(converted);
                Check(oldStrings.Text(oldStrings.Object("hkaAnimationBinding")+16).SequenceEqual(newStrings.Text(newStrings.Object("hkaAnimationBinding")+16)),"Native animation binding borrowed string changed");
                var op=new NativeAnimation.Pack(old.Bytes.ToArray());var np=new NativeAnimation.Pack(converted);int oa=op.Object("hkaSplineCompressedAnimation"),na=np.Object("hkaSplineCompressedAnimation");
                bool entrance=profile==BossEnemyCharacters.Taurus&&old.ID==1000007010;
                float duration=entrance?4.1666665f:op.Float(oa+16),delta=entrance?duration/(op.Int(oa+56)-1):op.Float(oa+80);
                Check(duration==np.Float(na+16)&&op.Int(oa+56)==np.Int(na+56)&&delta==np.Float(na+80),"Native action duration changed outside the explicit Taurus entrance");
                var od=op.Array(oa+152,1,64*1024*1024);var nd=np.Array(na+152,1,64*1024*1024);var os=op.Array(oa+88,4,256);var ns=np.Array(na+88,4,256);var oe=op.Array(oa+104,4,256);
                int tracks=oldRig.Names.Length,total=rig.Names.Length;
                var decoded=Spline.ReadSplineCompressedAnimByteBlock(false,converted.AsSpan(np.Data+nd.At,nd.Count).ToArray(),total,np.Int(na+60));
                int sourceID=BossEnemyCharacters.SourceSlot(profile,old.ID-1000000000);if(!cache.TryGetValue(sourceID,out var clip))cache.Add(sourceID,clip=SourceAnimation.Load(Source,profile.Source,sourceID));
                for(int block=0;block<os.Count;block++) {
                    int o=op.Data+od.At+op.Int(os.At+block*4),n=np.Data+nd.At+np.Int(ns.At+block*4),used=op.Int(oe.At+block*4);
                    Check(old.Bytes.Span.Slice(o,tracks*4).SequenceEqual(converted.AsSpan(n,tracks*4))&&old.Bytes.Span.Slice(o+tracks*4,used-tracks*4).SequenceEqual(converted.AsSpan(n+total*4,used-tracks*4)),"Original compressed native body channels changed");
                    int end=Math.Min(255,op.Int(oa+56)-1-block*255);
                    foreach(int frame in new[]{0,end/2,end}) {
                        var expected=old.Bytes.Span[op.Data+op.Object("hkaAnimationBinding")+80]==1?Enumerable.Repeat(Matrix4x4.Identity,clip.Bones.Count).ToArray():clip.Sample((block*255f+frame)/(op.Int(oa+56)-1)*clip.Duration);if(old.Bytes.Span[op.Data+op.Object("hkaAnimationBinding")+80]==0){Check(Matrix4x4.Invert(Sample(decoded[block][0],frame),out var rootInverse),"Native root is singular");expected[0]*=rootInverse;}
                        for(int bone=0;bone<expected.Length;bone++){float e=Error(expected[bone],Sample(decoded[block][tracks+bone],frame));maxPoseError=Math.Max(maxPoseError,e);Check(e<.0035f,"Authored source pose mismatch: "+profile.Source+"/"+sourceID+"/"+bone+" native="+old.ID+" frame="+frame+" blend="+old.Bytes.Span[op.Data+op.Object("hkaAnimationBinding")+80]+"="+e);}checkedPoses++;
                    }
                }
                // Existing animation and locomotion bytes remain in place; no
                // original data is removed when the appended arrays are added.
                int oldData=op.Data;Check(old.Bytes.Span.Slice(oldData,16).SequenceEqual(converted.AsSpan(np.Data,16)),"Native root container data changed");checkedClips++;
            }
            var payload=BossEnemyCharacters.Parameters(MapCharacters.Payload(nativeRows.Rows.Single(r=>r.ID==profile.Npc)),row,defs);var donorPayload=MapCharacters.Payload(nativeRows.Rows.Single(r=>r.ID==profile.Npc));
            for(int i=0;i<payload.Length;i++)if(i is not(>=16 and <24 or >=28 and <36))Check(payload[i]==donorPayload[i],"Boss parameter adaptation changed opaque native combat bytes");
            Check(BitConverter.ToSingle(payload,16)==Convert.ToSingle(row.Cells.Single(c=>c.Def.InternalName=="hitHeight").Value)&&BitConverter.ToSingle(payload,20)==Convert.ToSingle(row.Cells.Single(c=>c.Def.InternalName=="hitRadius").Value)&&BitConverter.ToSingle(payload,28)==Convert.ToSingle(row.Cells.Single(c=>c.Def.InternalName=="hitYOffset").Value)&&BitConverter.ToUInt32(payload,32)==Convert.ToUInt32(row.Cells.Single(c=>c.Def.InternalName=="hp").Value),"Source capsule/health lost");
            var aiBytes=MapNativeAi.Merge(Native("script/m24_00_00_00.luabnd.dcx"),Native("script/"+profile.Ai+".luabnd.dcx"),int.Parse(profile.Native[1..]));var ai=BND4.Read(aiBytes);var goals=LUAINFO.Read(ai.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo")).Bytes);var aiSource=BND4.Read(Native("script/"+profile.Ai+".luabnd.dcx"));
            foreach(var file in aiSource.Files.Where(f=>Leaf(f.Name).StartsWith(profile.Native[1..],StringComparison.Ordinal)&&Leaf(f.Name).EndsWith(".lua",StringComparison.Ordinal)))Check(ai.Files.Single(f=>Leaf(f.Name)==Leaf(file.Name)).Bytes.Span.SequenceEqual(file.Bytes.Span),"Native AI goal bytecode was lost or changed");
            var think=thinkRows.Rows.Single(r=>r.ID==profile.Think);int battleGoal=Convert.ToInt32(think.Cells.Single(c=>c.Def.InternalName=="battleGoalID").Value);Check(goals.Goals.Any(g=>g.ID==battleGoal),"Native controller battle goal registry is missing");
            var donorMap=MSBB.Read(Native("map/MapStudio/"+profile.Map+".msb.dcx"));Check(donorMap.Parts.Enemies.Any(p=>p.ModelName==profile.Native),"Native actor donor is absent");
            var serialized=BND4.Read(files["chr/"+target+".chrbnd.dcx"]);Check(serialized.Files.Single(f=>TPF.Is(f.Bytes.ToArray())).Bytes.Length>0,"Boss texture bank missing");var physics=BND4.Read(Native("chr/"+profile.Native+".chrbnd.dcx")).Files.Where(f=>Leaf(f.Name).EndsWith(".HKX",StringComparison.OrdinalIgnoreCase)&&!Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase));foreach(var file in physics)Check(file.Bytes.Span.SequenceEqual(serialized.Files.Single(f=>f.ID==file.ID).Bytes.Span),"Native physical collision binder payload changed");
            Directory.CreateDirectory(Path.Combine(args[3],target));foreach(var pair in files)File.WriteAllBytes(Path.Combine(args[3],target,Path.GetFileName(pair.Key)),pair.Value);
            result.Add(new{profile.Source,profile.Native,sourceBones=idle.Bones.Count,nativeBones=profile.NativeBones,totalBones=rig.Names.Length,clips=profile.Clips,maxVertexError,masks=masks.Order(),sourceClips=cache.Keys.Order(),health=BitConverter.ToUInt32(payload,32),capsule=new{height=BitConverter.ToSingle(payload,16),radius=BitConverter.ToSingle(payload,20),offset=BitConverter.ToSingle(payload,28)}});
            Console.WriteLine($"PASS {profile.Source}: {rig.Names.Length} bones, {profile.Clips} native/source clip pairs, source geometry/capsule/HP preserved");
        }
        Check(BossEnemyCharacters.SourceSlot(BossEnemyCharacters.Taurus,BossEnemyCharacters.TaurusWaitingAnimation)==9001&&BossEnemyCharacters.SourceSlot(BossEnemyCharacters.Taurus,BossEnemyCharacters.TaurusJumpAnimation)==9061,"Authored Taurus encounter poses lost");
        foreach(var(path,hash)in originals)Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash),"Original asset changed: "+path);
        Directory.CreateDirectory(args[3]);File.WriteAllText(Path.Combine(args[3],"validation.json"),JsonSerializer.Serialize(new{checkedClips,checkedPoses,maxPoseError,originalHashes=originals.ToDictionary(p=>p.Key,p=>Convert.ToHexString(p.Value)),result,limitations=BossEnemyCharacters.Limitations},new JsonSerializerOptions{IncludeFields=true,WriteIndented=true}));
        Console.WriteLine($"PASS {checkedClips} clips / {checkedPoses} independent poses; {originals.Count} original hashes unchanged");
    }
}

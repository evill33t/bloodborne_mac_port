// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// A versioned ownership manifest, independent of map import order. Never renumber
// an entry: copied mod saves retain these flags. Append new explicit reservations.
// These bits occupy a verified unused part of Bloodborne's existing52100 group.
internal static class MapQuestState {
    internal const int InitialFlag=52100499;
    internal static readonly IReadOnlyDictionary<int,int> Flags=new Dictionary<int,int> {
        [280]=52100100,
        [751]=52100101,
        [815]=52100102,
        [842]=52100103,
        [1092]=52100104,
        [1110]=52100105,
        [1112]=52100106,
        [1140]=52100107,
        [1141]=52100108,
        [1142]=52100109,
        [1143]=52100110,
        [1144]=52100111,
        [1145]=52100112,
        [1146]=52100113,
        [1147]=52100114,
        [1148]=52100115,
        [1149]=52100116,
        [1170]=52100117,
        [1171]=52100118,
        [1172]=52100119,
        [1173]=52100120,
        [1174]=52100121,
        [1175]=52100122,
        [1176]=52100123,
        [1177]=52100124,
        [1178]=52100125,
        [1179]=52100126,
        [1180]=52100127,
        [1181]=52100128,
        [1182]=52100129,
        [1183]=52100130,
        [1184]=52100131,
        [1185]=52100132,
        [1186]=52100133,
        [1187]=52100134,
        [1188]=52100135,
        [1189]=52100136,
        [1190]=52100137,
        [1191]=52100138,
        [1192]=52100139,
        [1193]=52100140,
        [1194]=52100141,
        [1195]=52100142,
        [1196]=52100143,
        [1197]=52100144,
        [1198]=52100145,
        [1199]=52100146,
        [1200]=52100147,
        [1201]=52100148,
        [1202]=52100149,
        [1203]=52100150,
        [1204]=52100151,
        [1205]=52100152,
        [1206]=52100153,
        [1207]=52100154,
        [1208]=52100155,
        [1209]=52100156,
        [1250]=52100157,
        [1252]=52100158,
        [1460]=52100159,
        [1461]=52100160,
        [1462]=52100161,
        [1463]=52100162,
        [1464]=52100163,
        [1465]=52100164,
        [1466]=52100165,
        [1467]=52100166,
        [1468]=52100167,
        [1469]=52100168,
        [1645]=52100169,
        [11010597]=52100170,
        [11010700]=52100171,
        [11010790]=52100172,
        [11010901]=52100173,
        [11020350]=52100174,
        [11020597]=52100175,
        [11020599]=52100176,
        [11020600]=52100177,
        [11020609]=52100178,
        [11026105]=52100179,
        [11026107]=52100180,
        [11026108]=52100181,
        [11400200]=52100182,
        [71020020]=52100183,
        [71020021]=52100184,
        [71020022]=52100185,
        [71020023]=52100186,
        [71020026]=52100187,
        [71020027]=52100188,
        [71020028]=52100189,
        [71020029]=52100190,
        [71100020]=52100191,
        [71100021]=52100192,
        [71100022]=52100193,
        [71100023]=52100194,
        [71100025]=52100195,
        [71100026]=52100196,
        [71100027]=52100197,
        [71100028]=52100198,
        [71100029]=52100199,
        [71100030]=52100200,
        [71100031]=52100201,
        [71100032]=52100202,
        [71100035]=52100203,
        [71100036]=52100204,
        [71100037]=52100205,
        [71100038]=52100206,
        [71100039]=52100207,
        [71100040]=52100208,
        [71100041]=52100209,
        [71100042]=52100210,
        [71100043]=52100211,
        [71100044]=52100212,
        [71100045]=52100213,
        [71100046]=52100214,
        [71100047]=52100215,
        [71100048]=52100216,
        [71100049]=52100217,
        [71100050]=52100218,
        [71100051]=52100219,
        [71100052]=52100220,
        [71100053]=52100221,
        [71100055]=52100222,
        [71300055]=52100223,
        [71300056]=52100224,
        [71300057]=52100225,
        [71300058]=52100226,
        [71300059]=52100227,
        [71300060]=52100228,
        [71300061]=52100229,
        [71300062]=52100230,
        [71300063]=52100231,
        [71300064]=52100232,
        [71300065]=52100233,
        [71300066]=52100234,
        [71300067]=52100235,
        [71300068]=52100236,
        [71300069]=52100237,
        [71300070]=52100238,
        [71300071]=52100239,
        [71300072]=52100240,
        [71300073]=52100241,
        [71300074]=52100242,
        [71300075]=52100243,
        [71300076]=52100244,
        [71300077]=52100245,
        [71300078]=52100246,
        [71300079]=52100247,
        [71300081]=52100248,
        [71300083]=52100249,
        [71300084]=52100250,
        [71300085]=52100251,
        [71300086]=52100252,
        [71300087]=52100253,
        [71300090]=52100254,
        [71300091]=52100255,
        [71300092]=52100256,
        [71300093]=52100257,
        [1150]=52100259,
        [1151]=52100260,
        [1152]=52100261,
        [1153]=52100262,
        [1154]=52100263,
        [1155]=52100264,
        [1156]=52100265,
        [1157]=52100266,
        [1158]=52100267,
        [1159]=52100268,
        [1160]=52100269,
        [1161]=52100270,
        [1162]=52100271,
        [1163]=52100272,
        [1164]=52100273,
        [1165]=52100274,
        [1166]=52100275,
        [1167]=52100276,
        [1168]=52100277,
        [1169]=52100278,
        [1470]=52100279,
        [1471]=52100280,
        [1472]=52100281,
        [1473]=52100282,
        [1474]=52100283,
        [1475]=52100284,
        [1476]=52100285,
        [1477]=52100286,
        [1478]=52100287,
        [1479]=52100288,
        [1480]=52100289,
        [1481]=52100290,
        [1482]=52100291,
        [1483]=52100292,
        [1484]=52100293,
        [1485]=52100294,
        [1486]=52100295,
        [1487]=52100296,
        [1488]=52100297,
        [1489]=52100298,
    };
    internal static int Flag(int source)=>Flags.TryGetValue(source,out int target)?target:
        throw new NotSupportedException("Source quest flag has no stable reservation: "+source);

    internal static void VerifyReservations(string nativeRoot) {
        var reserved=Flags.Values.Append(InitialFlag).ToHashSet();
        if(reserved.Count!=Flags.Count+1)throw new InvalidDataException("Imported quest flag ownership aliases");
        void Scan(ReadOnlySpan<byte> bytes,string owner) {
            for(int at=0;at+4<=bytes.Length;at++) {
                int value=System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(bytes[at..]);
                if(reserved.Contains(value))throw new InvalidDataException("Imported quest flag is occupied by native data: "+value+" ("+owner+")");
            }
        }
        foreach(var file in BND4.Read(Path.Combine(nativeRoot,"param/gameparam/gameparam.parambnd.dcx")).Files)Scan(file.Bytes.Span,file.Name);
        foreach(string folder in new[]{"event","script/talk"})foreach(string path in Directory.EnumerateFiles(Path.Combine(nativeRoot,folder),"*.dcx")) {
            var bytes=File.ReadAllBytes(path);var owner=Path.GetRelativePath(nativeRoot,path);
            // Talk binders may compress each member independently of the outer
            // DCX. Scan decoded members so those flags cannot escape the guard.
            if(BND4.Is(bytes))foreach(var file in BND4.Read(bytes).Files)Scan(file.Bytes.Span,owner+":"+file.Name);
            else Scan(DCX.Is(bytes)?DCX.Decompress(bytes).Span:bytes,owner);
        }
    }
    static byte[] Args(params object[] args) {
        using var stream=new MemoryStream();using var writer=new BinaryWriter(stream);
        foreach(var a in args) {
            int size=a is byte or sbyte?1:4;while(stream.Position%size!=0)writer.Write((byte)0);
            switch(a){case byte n:writer.Write(n);break;case sbyte n:writer.Write(n);break;case int n:writer.Write(n);break;case uint n:writer.Write(n);break;default:throw new ArgumentException("Unsupported quest event argument");}
        }
        while(stream.Position%4!=0)writer.Write((byte)0);return stream.ToArray();
    }
    static EMEVD.Instruction Ins(int bank,int command,params object[] args)=>new(bank,command,Args(args));
    static void Prefix(EMEVD.Event e,List<EMEVD.Instruction> prefix) {
        foreach(var p in e.Parameters)p.InstructionIndex+=prefix.Count;
        e.Instructions.InsertRange(0,prefix);
    }
    internal static byte[] Initialize(byte[] native,byte[] sourceCommon) {
        var common=EMEVD.Read(sourceCommon).Events.Single(e=>e.ID==50);
        foreach(var (at,first,last,fresh) in new[]{(13,1140,1169,1140),(17,1190,1209,1202),(53,1460,1489,1460)}) {
            var expected=new[]{Ins(1003,3,(byte)1,(byte)2,(byte)0,first,last),Ins(2003,2,fresh,(byte)1)};
            if(common.Instructions.Count<at+2||!common.Instructions.Skip(at).Take(2).Zip(expected).All(p=>p.First.Bank==p.Second.Bank&&p.First.ID==p.Second.ID&&p.First.ArgData.AsSpan().SequenceEqual(p.Second.ArgData)))
                throw new InvalidDataException("Source initial NPC quest states changed");
        }
        var script=EMEVD.Read(native);
        if(script.Format!=EMEVD.Game.Bloodborne)throw new InvalidDataException("Quest state requires native Bloodborne events");
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);
        if(pre==null){pre=new EMEVD.Event(50);script.Events.Add(pre);}
        // The source common preconstructor's fresh NPC states. Only initialize
        // empty source state ranges, preserving later progression on mod saves.
        var prefix=new List<EMEVD.Instruction>();
        foreach(var (first,last,fresh) in new[]{(1140,1169,1140),(1190,1209,1202),(1460,1489,1460)}) {
            // Individual branches support disjoint stable reservations without
            // leaving condition-group state behind in the native preconstructor.
            foreach(int flag in Enumerable.Range(first,last-first+1))prefix.Add(Ins(1003,1,checked((byte)(last-flag+1)),(byte)1,(byte)0,Flag(flag)));
            prefix.Add(Ins(2003,2,Flag(fresh),(byte)1));
        }
        prefix.Insert(0,Ins(1003,1,checked((byte)(prefix.Count+1)),(byte)1,(byte)0,InitialFlag));
        prefix.Add(Ins(2003,2,InitialFlag,(byte)1));
        Prefix(pre,prefix);
        return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }

    internal record Presence(int SourceEntity,int TargetEntity,int EventID,int[] AbsentFlags);
    internal static (byte[] Script,IReadOnlyList<Presence> Bindings) Availability(byte[] native,
        IReadOnlyDictionary<int,int> actorEntities,byte[] sourceScript) {
        var source=EMEVD.Read(sourceScript);var sourcePre=source.Events.Single(e=>e.ID==50);
        // Source11020110 protects Anastacia before its later death/resurrection
        // logic. Retain only that initial protection on the imported actor.
        var immortalEvents=source.Events.Where(e=>e.ID==11020110).ToArray();var immortal=Ins(2004,12,6060,(byte)1);
        if(immortalEvents.Length!=1||immortalEvents[0].Instructions.Count==0||immortalEvents[0].Parameters.Any(p=>p.InstructionIndex==0))
            throw new InvalidDataException("Source Anastacia initial immortality contract changed");
        var first=immortalEvents[0].Instructions[0];
        if(first.Bank!=immortal.Bank||first.ID!=immortal.ID||first.Layer!=null||!first.ArgData.AsSpan().SequenceEqual(immortal.ArgData))
            throw new InvalidDataException("Source Anastacia initial immortality contract changed");
        if(!actorEntities.TryGetValue(6060,out int anastacia)||anastacia<=0)throw new InvalidDataException("Missing initial Firelink NPC: 6060");
        // Verify the actual preconstructor selection, including its source skips.
        var contracts=new[]{(Entity:6080,At:104,Code:new[]{Ins(1003,1,(byte)1,(byte)1,(byte)0,1192),Ins(1000,3,(byte)1),Ins(2004,5,6080,(byte)0)},Flags:new[]{1192}),
            (Entity:6270,At:146,Code:new[]{Ins(1003,1,(byte)2,(byte)1,(byte)0,1464),Ins(1003,1,(byte)1,(byte)1,(byte)0,1462),Ins(1000,3,(byte)1),Ins(2004,5,6270,(byte)0)},Flags:new[]{1462,1464})};
        var script=EMEVD.Read(native);var pre=script.Events.Single(e=>e.ID==50);var ctor=script.Events.Single(e=>e.ID==0);
        var initial=new List<EMEVD.Instruction>{Ins(2004,12,anastacia,(byte)1)};var bindings=new List<Presence>();
        foreach(var c in contracts) {
            if(!sourcePre.Instructions.Skip(c.At).Take(c.Code.Length).Zip(c.Code).All(p=>p.First.Bank==p.Second.Bank&&p.First.ID==p.Second.ID&&p.First.ArgData.AsSpan().SequenceEqual(p.Second.ArgData))||sourcePre.Instructions.Count<c.At+c.Code.Length)
                throw new InvalidDataException("Source Firelink NPC availability contract changed: "+c.Entity);
            if(!actorEntities.TryGetValue(c.Entity,out int entity)||entity<=0)throw new InvalidDataException("Missing initial Firelink NPC: "+c.Entity);
            int eventID=12415810+bindings.Count;var flags=c.Flags.Select(Flag).ToArray();
            if(script.Events.Any(e=>e.ID==eventID))throw new InvalidDataException("NPC availability event namespace occupied");
            initial.Add(Ins(2004,5,entity,(byte)1));
            foreach(int flag in flags){initial.Add(Ins(1003,1,(byte)1,(byte)0,(byte)0,flag));initial.Add(Ins(2004,5,entity,(byte)0));}
            var e=new EMEVD.Event(eventID,EMEVD.Event.RestBehaviorType.Restart);var code=e.Instructions;
            foreach(int flag in flags)code.Add(Ins(3,0,(sbyte)-1,(byte)1,(byte)0,flag));
            code.Add(Ins(1000,1,checked((byte)(flags.Length+3)),(byte)0,(sbyte)-1));
            code.Add(Ins(2004,5,entity,(byte)0));
            foreach(int flag in flags)code.Add(Ins(3,0,(sbyte)1,(byte)0,(byte)0,flag));
            code.Add(Ins(0,0,(sbyte)0,(byte)1,(sbyte)1));code.Add(Ins(1000,4,(byte)1));
            code.Add(Ins(2004,5,entity,(byte)1));
            foreach(int flag in flags)code.Add(Ins(3,0,(sbyte)-2,(byte)1,(byte)0,flag));
            code.Add(Ins(0,0,(sbyte)0,(byte)1,(sbyte)-2));code.Add(Ins(1000,4,(byte)1));
            script.Events.Add(e);ctor.Instructions.Add(Ins(2000,0,0,(uint)eventID,0u));bindings.Add(new(c.Entity,entity,eventID,flags));
        }
        Prefix(pre,initial);return(script.Write(DCX.Type.DCX_DFLT_10000_44_9),bindings);
    }
}

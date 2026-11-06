// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// Append-only expansion into existing native collection groups. The native
// initializer/reader/writer use kind1 slot196 for52601 and slot193 for52411.
// Source IDs retain their identity across map order and previous mod saves.
internal static class MapNpcQuestState {
    internal const int InitialFlag=52601999;
    internal static readonly (int At,int First,int Last,int Fresh)[] Ranges=[
        (1,1000,1029,1000),
        (3,1030,1059,1030),
        (5,1060,1089,1060),
        (7,1090,1109,1090),
        (9,1110,1119,1110),
        (11,1120,1139,1120),
        (13,1140,1169,1140),
        (15,1170,1189,1170),
        (17,1190,1209,1202),
        (19,1210,1219,1210),
        (21,1220,1229,1220),
        (23,1230,1239,1230),
        (25,1240,1249,1240),
        (27,1250,1259,1250),
        (29,1270,1279,1270),
        (31,1280,1289,1280),
        (33,1290,1309,1290),
        (35,1310,1319,1310),
        (37,1320,1339,1320),
        (39,1340,1359,1340),
        (41,1360,1379,1360),
        (43,1380,1399,1380),
        (45,1400,1409,1400),
        (47,1410,1419,1410),
        (49,1420,1429,1420),
        (51,1430,1459,1430),
        (53,1460,1489,1460),
        (55,1490,1539,1490),
        (57,1540,1569,1540),
        (59,1570,1599,1570),
        (61,1600,1619,1600),
        (63,1620,1639,1620),
        (65,1640,1669,1640),
        (67,1670,1679,1670),
        (69,1690,1699,1690),
        (71,1700,1709,1700),
        (73,1710,1729,1710),
        (75,1760,1769,1760),
        (77,1770,1779,1770),
        (79,1780,1789,1780),
        (81,1820,1839,1820),
        (83,1840,1859,1840),
        (85,1860,1869,1860),
        (87,1870,1889,1870),
    ];
    internal static readonly IReadOnlyDictionary<int,int> Flags=Reservations();
    static IReadOnlyDictionary<int,int> Reservations() {
        var flags=Enumerable.Range(1000,890).Where(f=>!MapQuestState.Flags.ContainsKey(f)).ToDictionary(f=>f,f=>52601100+f-1000);
        int[] dialogue=[11010590,11010591,11010594,71010050,71010051,71010052,71010053,71010054,71010055,71010056,71010057,71010058];
        for(int i=0;i<dialogue.Length;i++)flags.Add(dialogue[i],52411100+i);
        flags.Add(11010860,52411112); // Havel: append-only persistent death.
        flags.Add(11200801,52411113); // Basin Hydra defeat.
        flags.Add(11200800,52411114); // Dusk's golden golem defeat.
        flags.Add(703,52411115); // Source new-visit gate after Hydra.
        flags.Add(6,52411116); // Pinwheel defeat; source global boss flag.
        flags.Add(11300000,52411117); // Pinwheel introduction seen.
        for(int i=0;i<7;i++)flags.Add(11205300+i,52411120+i); // Individual severed heads.
        for(int i=0;i<10;i++)flags.Add(11300850+i,52411130+i); // Six necromancers, two lizards, Titanite Demon and Black Knight.
        int[] dusk=[71200001,71200002,11200590,11200591];for(int i=0;i<dusk.Length;i++)flags.Add(dusk[i],52411140+i);
        flags.Add(71200000,52411144); // Dusk introduction completed.
        int[] patches=[11300403,11305037,11305038,11300592,11300593,71310066,71310067,71310076,71310077];for(int i=0;i<patches.Length;i++)flags.Add(patches[i],52411150+i);
        int[] patchesGift=[71310068,11300590,71310070,71310071,71310072,11300591];for(int i=0;i<patchesGift.Length;i++)flags.Add(patchesGift[i],52411159+i);
        flags.Add(11010181,52411165); // Residence door: Griggs rescue prerequisite.
        // 52411166 belongs to the door's key-eligibility controller.
        int[] griggs=[71010035,71010040,71020067];for(int i=0;i<griggs.Length;i++)flags.Add(griggs[i],52411167+i);
        int[] oswald=[11010581,71800057,11016111,71800058];for(int i=0;i<oswald.Length;i++)flags.Add(oswald[i],52411170+i);
        flags.Add(71010000,52411174); // Andre introduction completed.
        // Shop stock is a numeric counter, not a Boolean. The old52411175
        // overlapped the following gate/Thorolund flags and appeared sold out.
        // Keep that old bit unused; reserve52411400..431 for the counter.
        flags.Add(11017140,52411400); // Andre's one-stock Crest of Artorias.
        flags.Add(11200100,52411176); // Garden Crest gate opened.
        flags.Add(11200101,52411177); // Sif approach gate opened.
        int[] thorolund=[11020560,11020692,11020693,11300005,71020080,71310023,71310024,71310025,11026109,71310032,71310033,11026110];
        for(int i=0;i<thorolund.Length;i++)flags.Add(thorolund[i],52411178+i);
        return flags;
    }
    internal static int Flag(int source) {
        // The nested source event and the outer caster-death event represent
        // the same six deaths. Normalize that explicit alias before ownership
        // lookup; the reservation dictionary itself must stay one-to-one.
        if(source is >=11300801 and <=11300806)source=11300850+source-11300801;
        return MapQuestState.Flags.TryGetValue(source,out int old)?old:Flags.TryGetValue(source,out int owned)?owned:throw new NotSupportedException("Unreserved source NPC flag: "+source);
    }
    static EMEVD.Instruction Ins(int bank,int id,params object[] args)=>new(bank,id,args);
    static bool Same(EMEVD.Instruction a,EMEVD.Instruction b)=>a.Bank==b.Bank&&a.ID==b.ID&&a.Layer==b.Layer&&a.ArgData.SequenceEqual(b.ArgData);
    internal static byte[] Initialize(byte[] native,byte[] sourceCommon) {
        var common=EMEVD.Read(sourceCommon).Events.Single(e=>e.ID==50);
        foreach(var r in Ranges) {
            EMEVD.Instruction[] expected=[Ins(1003,3,(byte)1,(byte)2,(byte)0,r.First,r.Last),Ins(2003,2,r.Fresh,(byte)1)];
            if(common.Instructions.Count<r.At+2||!common.Instructions.Skip(r.At).Take(2).Zip(expected).All(p=>Same(p.First,p.Second)))throw new InvalidDataException("Source NPC initial range changed: "+r.First);
        }
        var script=EMEVD.Read(native);if(script.Format!=EMEVD.Game.Bloodborne)throw new InvalidDataException("NPC initialization requires native events");
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}
        var prefix=new List<EMEVD.Instruction>();
        foreach(var r in Ranges) {
            // A separate short branch per range avoids an eight-bit skip across
            // the full initializer and preserves native preconstructor execution.
            prefix.Add(Ins(1003,1,checked((byte)(r.Last-r.First+2)),(byte)1,(byte)0,InitialFlag));
            for(int f=r.First;f<=r.Last;f++)prefix.Add(Ins(1003,1,checked((byte)(r.Last-f+1)),(byte)1,(byte)0,Flag(f)));
            prefix.Add(Ins(2003,2,Flag(r.Fresh),(byte)1));
        }
        prefix.Add(Ins(1003,1,(byte)1,(byte)1,(byte)0,InitialFlag));prefix.Add(Ins(2003,2,InitialFlag,(byte)1));
        foreach(var p in pre.Parameters)p.InstructionIndex+=prefix.Count;pre.Instructions.InsertRange(0,prefix);
        return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}

// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// Explicit DS1 goods semantics. Equipment IDs must never pass through to a
// different game's inventory merely because their numeric IDs happen to exist.
internal static class MapItemPolicy {
    internal record Conversion(int SourceID,int TargetID,int Count,string Kind,string Description);
    internal const int SourceGoodsCategory=0x40000000;
    internal const int NativeGoodsCategory=4;
    internal const int BloodVial=1000,MadmansKnowledge=1500,GreatOnesWisdom=1501;
    internal static int QuestID(int sourceID) {
        if(sourceID<0||sourceID>99999)throw new InvalidDataException("Source quest item exceeds its reserved namespace");
        return 900000+sourceID;
    }
    static readonly Dictionary<int,int> Equivalents=new() {
        [117]=100,[230]=703,[240]=703,[270]=1101,[271]=1100,[272]=1100,
        [290]=1240,[291]=1210,[292]=1200,[294]=1260,[296]=1270,[297]=1201,
        [310]=1300,[311]=1320,[330]=1400,[350]=MadmansKnowledge,
        [370]=1410,[371]=1411,[500]=MadmansKnowledge,[501]=GreatOnesWisdom,
        [1000]=3000,[1010]=3010,[1020]=3010,[1030]=3020,[1040]=3020,
        [1050]=3020,[1060]=3020,[1070]=3030,[1080]=3030,[1090]=3030,[1100]=3030
    };
    internal static bool IsQuest(PARAM.Row row) {
        int kind=Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="goodsType").Value);
        return kind==1||row.ID is 115 or 116 or 220 or 374 or 375 or 376 or 384 or >=390 and <=396 or >=700 and <=711 or 1110 or 1120 or 1130;
    }
    internal static Conversion? Map(int category,int id,int count,bool quest) {
        if(count<0||count>99)throw new InvalidDataException("Source item quantity exceeds the native stack budget");
        if(category!=SourceGoodsCategory||count==0||id<=0)return null;
        if(quest)return new(id,QuestID(id),count,"quest-item","Independent inventory identity; source effects and quest conditions require their own adapter.");
        if(id is >=200 and <=215)return new(id,BloodVial,Math.Min(99,count*5),"native-equivalent","Estus acquisition becomes five Blood Vials per flask; native healing and storage rules apply.");
        if(id is >=400 and <=409)return new(id,1510+id-400,count,"native-equivalent","Source soul tier becomes the corresponding Coldblood tier; native Blood Echo values apply.");
        return Equivalents.TryGetValue(id,out int target)?new(id,target,count,"native-equivalent",
            id is 350 or 500?"Humanity becomes Madman's Knowledge; consuming it grants native Insight.":
            id==501?"Twin Humanities becomes Great One's Wisdom; consuming it grants native Insight.":
            "Native consumable equivalent; source effects and balance are approximate."):null;
    }
}

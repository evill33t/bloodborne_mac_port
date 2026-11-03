// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text.Json;
static class HydraEventsTests {
 static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
 static void Need(bool b,string s){if(!b)throw new Exception(s);}
 static void Main(string[] args) {
  var source=File.ReadAllBytes(Path.Combine(args[0],"event/m12_00_00_00.emevd.dcx"));
  var native=new EMEVD(EMEVD.Game.Bloodborne);native.Events.Add(new EMEVD.Event(0));native.Events.Add(new EMEVD.Event(50));
  var effects=Enumerable.Range(0,7).ToDictionary(i=>5430+i,i=>883200+i);
  var result=HydraEvents.Apply(native.Write(),source,effects);var script=EMEVD.Read(result.Script);
  Need(result.Heads.Count==7&&result.Heads[^1].Group==8&&result.Heads.Select(h=>h.HP).Distinct().Single()==176,"Headownership lost");
  var schema=JsonDocument.Parse(File.ReadAllText("out/map-editor/skeleton-animation/native-investigation/native-emedf.json")).RootElement.EnumerateArray().ToDictionary(v=>(v.GetProperty("bank").GetInt32(),v.GetProperty("id").GetInt32()));
  foreach(var e in script.Events)foreach(var i in e.Instructions)Need(schema[(i.Bank,i.ID)].GetProperty("argLength").GetInt32()==i.ArgData.Length,$"Native ABI mismatch {i.Bank}[{i.ID}]");
  foreach(var h in result.Heads) {
   var e=script.Events.Single(e=>e.ID==HydraEvents.FirstEvent+h.Slot);
   var create=e.Instructions.Single(i=>i.Bank==2004&&i.ID==22).ArgData;
   Need(BitConverter.ToInt16(create,4)==h.SourcePart&&BitConverter.ToInt16(create,6)==h.Group,"Part group wrong");
   var warp=e.Instructions.Single(i=>i.Bank==2004&&i.ID==42).ArgData;
   Need(BitConverter.ToInt32(warp,12)==h.TargetSocket&&BitConverter.ToInt32(warp,8)==HydraEvents.Body,"Sever socket wrong");
   Need(e.Instructions.Any(i=>i.Bank==2003&&i.ID==2&&BitConverter.ToInt32(i.ArgData)==h.Flag),"Head persistence missing");
  }
  Need(script.Events.Single(e=>e.ID==HydraEvents.FirstEvent+7).Instructions.Count(i=>i.Bank==3&&i.ID==0)==7,"Allheads completion contract lost");
  try{HydraEvents.Apply(result.Script,source,effects);throw new Exception("Duplicate namespace accepted");}catch(InvalidDataException){}
  if(args.Length>1)NativeEventReservations.Verify(args[1],12416000,600);
  Console.WriteLine("PASS Hydra events: 7 source slots resolved, partgroups/masks/severs/persistence, native command ABI and namespace rejection");
 }
}

// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
string root=Directory.Exists(Path.Combine(args[0],"dvdroot_ps4"))?Path.Combine(args[0],"dvdroot_ps4"):args[0];var hashes=new Dictionary<string,byte[]>();var files=new Dictionary<string,byte[]>();
void Check(bool v,string m){if(!v)throw new Exception(m);}
byte[]? Read(string p){if(files.TryGetValue(p,out var b))return b;if(!File.Exists(Path.Combine(root,p)))return null;var bytes=File.ReadAllBytes(Path.Combine(root,p));hashes.TryAdd(p,SHA256.HashData(bytes));return bytes;}
void Write(string p,byte[] b){Check(!p.StartsWith("map/MapStudio/"),"Seed overwrote native MSB");files[p]=b;}
foreach(var (source,target) in new[]{("m10_02_00_00","m40_02_00_00"),("m10_01_00_00","m40_03_00_00")}){
 var choice=MapDestination.Select("additional","DS1R",source,target);var donor=MSBB.Read(Read("map/MapStudio/"+choice.TemplateMap+".msb.dcx")!);var before=donor.Write();var map=MapDestination.Create(donor,choice);
 Check(map.Parts.Players.Count==1&&map.Parts.Collisions.Count==1&&map.Parts.MapPieces.Count==0&&map.Parts.Enemies.Count==0&&map.Events.GetEntries().Count==0&&map.Regions.GetEntries().Count==0,"Additional scene imported native residents/events");
 MapDestination.Seed(choice,Read,Write);var script=EMEVD.Read(files["event/"+target+".emevd.dcx"]);Check(script.Events.Select(e=>e.ID).Order().SequenceEqual(new long[]{0,50})&&script.Events.All(e=>e.Instructions.Count==0),"Fresh event roots changed");
 foreach(string kind in new[]{"_envmap","_cgrading"}){var old=TPF.Read(Read("map/m24/m24"+kind+".tpf.dcx")!);var added=TPF.Read(files["map/m40/m40"+kind+".tpf.dcx"]);Check(old.Textures.Count==added.Textures.Count,"Environment lost textures");for(int i=0;i<old.Textures.Count;i++){Check(added.Textures[i].Name==old.Textures[i].Name.Replace("m24_","m40_"),"Environment internal key not remapped");Check(added.Textures[i].Bytes.AsSpan().SequenceEqual(old.Textures[i].Bytes),"Environment texels changed");}}
 string talkPath="script/talk/"+target+".talkesdbnd.dcx";var talk=BND4.Read(files[talkPath]);var e=ESD.Read(talk.Files[0].Bytes);e.Name=target=="m40_02_00_00"?"t402970":"t403970";talk.Files.Add(new BinderFile(Binder.FileFlags.Flag1,talk.Files.Max(f=>f.ID)+1,e.Name+".esd",e.Write()));files[talkPath]=talk.Write(DCX.Type.DCX_DFLT_10000_44_9);
 var actor=new MSBB.Part.Enemy{Name="c8900_0000",ModelName="c8900",EntityID=target=="m40_02_00_00"?2419700:4039000,TalkID=int.Parse(e.Name[1..]),CollisionName=map.Parts.Collisions[0].Name};map.Models.Enemies.Add(new MSBB.Model.Enemy("c8900"));map.Parts.Enemies.Add(actor);
 MapDestination.Finish(map,choice,Read,Write);Check(!map.Parts.Objects.Any(p=>p.Name==MapDestination.UrnSeed)&&map.Parts.Objects.Count==0&&BND4.Read(files[talkPath]).Files.Count==1,"Template actor/talk leaked into additional scene");Check(before.SequenceEqual(donor.Write()),"Donor map mutated");
 var tex=MapDestination.TextureArchive(choice,Read);Check(tex.Files.Count==(target=="m40_02_00_00"?0:1),"Texture composition lost earlier entries or copied native terrain");
 if(tex.Files.Count==1)Check(tex.Files[0].Name=="m40_import_first.tpf.dcx"&&tex.Files[0].Bytes.Span.SequenceEqual(new byte[]{1,2,3}),"Earlier imported texture was changed");
 tex.Files.Add(new BinderFile(Binder.FileFlags.Flag1,tex.Files.Count,target=="m40_02_00_00"?"m40_import_first.tpf.dcx":"m40_import_second.tpf.dcx",new byte[]{1,2,3}));tex.Write(out var th,out var td);files["map/m40/m40_0003.tpfbhd"]=th;files["map/m40/m40_0003.tpfbdt"]=td;
 MapDestination.VerifyNativeEntityReservations(root,map);
 if(target=="m40_03_00_00"){actor.EntityID=2409000;bool rejected=false;try{MapDestination.VerifyNativeEntityReservations(root,map);}catch(InvalidDataException){rejected=true;}Check(rejected,"Native Burg2409000 conflict escaped the global guard");actor.EntityID=4039000;}
 Console.WriteLine("Destination PASS "+target);
}
void Reject(Action action){try{action();}catch(NotSupportedException){return;}throw new Exception("Unsupported destination accepted");}
Reject(()=>MapDestination.Select("additional","DS2S","m10_04_00_00","m40_02_00_00"));Reject(()=>MapDestination.Select("additional","DS1R","m10_01_00_00","m40_02_00_00"));Reject(()=>MapDestination.Select("replacement","DS1R","m10_02_00_00","m40_02_00_00"));
var replacement=MapDestination.Select("replacement","DS1R","m10_02_00_00","m24_01_00_01");Check(!replacement.Additional&&replacement.TemplateMap=="m24_01_00_01","Replacement selection changed");
byte[] prefix=[0xff,0x82,0xa0,13,10];var appended=MapDestination.AppendLoadList(prefix,"m40_02_00_00");Check(appended.AsSpan(0,prefix.Length).SequenceEqual(prefix)&&appended.SequenceEqual(MapDestination.AppendLoadList(appended,"m40_02_00_00")),"Area list changed original bytes or duplicated a map");
var loadlist=System.Text.Encoding.ASCII.GetString(files["map/m40/m40.arealoadlist"]);Check(loadlist.Contains("m40_02")&&loadlist.Contains("m40_03"),"Composed area list lost earlier map");foreach(var (p,h) in hashes)Check(h.SequenceEqual(SHA256.HashData(File.ReadAllBytes(Path.Combine(root,p)))),"Original mutated: "+p);Console.WriteLine("PASS source hashes "+hashes.Count);

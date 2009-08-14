package nbench.common.helper;
import nbench.common.*;
import java.util.Map;
import java.util.Set;
import java.util.HashSet;
import java.util.Iterator;

//
// ���� scope���� backend sqlmap statement�� inputmap�� ������ �Ѵ�
// ���� backend client������ variable scope�� ���� map�� ������ �ϹǷ�
// ���ڰ��� ��ȯ�� �����ο� variable scope�� ���� �Ѱ��ش�.
//
public class MapVariableScope implements VariableScope
{
  private Map map;
  public MapVariableScope(Map map) 
  {
    this.map = map;
  }
  public Map getMap() 
  { 
    return map;
  }
  /* -------------------------------------- */
  /* VariableScope interface implementation */
  /* -------------------------------------- */
  public Variable getVariable(final String name)
  {
	  /* debug */
	  /*
	  int mapsize = map.size();
	  Set set = map.keySet();
	  
	  Iterator it = set.iterator();
	  int i = 0;
	  while(it.hasNext())
	  {
		  String lname = (String)it.next();
		System.out.println("MAP KEY " + i + "(" + name +"): " + lname + "(value:" + map.get(lname)+ ")");
		i++;
	  }
	  */
	  /* end debug */
	  
    Object obj;
    final NValue nval;
    if((obj = map.get(name)) == null)
      return null;
    try { 
      nval = new NValue(obj);
    }
    catch(Exception e) {
      System.out.println(e.toString()); //TODO exceptin handling
      return null;
    }
    return new Variable()
    {
      String n = name;
      NValue v = nval;
      public Value getValue() { return v; }
      public void setValue(Value val) {}
      public String getName() { return n; }
      public int getType() { return v.getType(); }
    };
  }
  public Set<String> getVariableNames()
  {
	Set<String> set = new HashSet<String>();
    for(Object o : map.keySet())
      set.add(o.toString());
    return set;
  }
}

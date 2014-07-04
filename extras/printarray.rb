class PrintableArray < Array
  def dump(line_max, indent)
    print("\t" * indent, "{\n")
    max = self.length - 1
    prefix = "\t" * (indent + 1)
    self.each_with_index do |v, i|
      e = sprintf("0x%02x", v)
      mod = i % line_max
      if mod == 0 then
        print(prefix, e, ",")
      elsif i == max then
        print(" ", e, "\n")
      elsif mod == line_max - 1 then
        print(" ", e, ",\n")
      else
        print(" ", e, ",")
      end
    end
    print("\n") if (line_max < self.length) && ((self.length % line_max) != 0)
    print("\t" * indent, "}")
  end
end
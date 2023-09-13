circuit Example :
  module Example :
    input clock : Clock
    input reset : UInt<1>
    input in : UInt<1> @[src/main/scala/main.scala 7:14]
    output out : UInt @[src/main/scala/main.scala 8:21]

    when in : @[src/main/scala/main.scala 9:13]
      out <= UInt<7>("h64") @[src/main/scala/main.scala 10:9]
    else :
      out <= UInt<2>("h2") @[src/main/scala/main.scala 12:9]


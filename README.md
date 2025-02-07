# bmailer
self-hosted software for newsletters

There are multiple channels. Subscribers are identified by their email
address, and only log in via passwordless sign-on links.

When logged in, they get a list of channels they are subscribed to. 
There are also hidden channels, only visible to users cleared to see them.

Messages are submitted for potential sending. They can be test-sent to a
specific email address, or to a channel.

Messages are exclusively generated using Markdown. To this is added an
unsubscribe link.

# Roadmap
Initially we start with a command line tool. 

$ bmail channel create "Tech updates"
Created channel c1

$ bmail mail create bertmail.md
Imported email e1

$ bmail mail send e1 bert@hubertnet.nl
sent

$ bmail channel add-user bert.hubert@example.com c1
created user bert.hubert@example.com
added user bert.hubert@ecample.com to "Tech updates" (c1)

$ bmail send e1 c1
Sent email e1 to channel c1
0 immediate errors

# Concept
We send mail to our smart host. We record immediate problems.
Bounces arrive in a mailbox which we poll periodically. 

Email messages are sent individually from unique envelope sender.

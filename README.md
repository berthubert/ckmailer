# ckmailer
self-hosted software for newsletters

There are multiple channels. Subscribers are identified by their email
address, and only log in via passwordless sign-on links.

When logged in, they get a list of channels they are subscribed to. 
There are also hidden channels, only visible to users cleared to see them.

Messages are submitted for potential sending. They can be test-sent to a
specific email address, or to a channel.

Messages are exclusively generated using Markdown. To this is added an
unsubscribe link.

# Status
"works for me", there is no real documentation, you'll likely struggle to
understand how this works for now.

# Requirements

Very new pandoc.

Links browser.



# Roadmap
Initially we start with a command line tool. 

# Concept
We send mail to our smart host. We record immediate problems.
Bounces arrive in a mailbox which we poll periodically. 

Email messages are sent individually from unique envelope sender.

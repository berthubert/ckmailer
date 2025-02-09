"use strict";

async function doRequestAccountPage(f)
{
    const formData = new FormData();
    formData.append('email', f.email);

    const response = await fetch('send-user-account-link', { method: "POST", body: formData });
    if (response.ok === true) {
        const data = await response.json();
	console.log(data);
	f.state = 1;
    }
    else {
	console.log("error");
    }
    return false;
}


async function changeSubscription(timsi, channelid, el)
{
    console.log(el.checked);
    console.log(el);
    if(el.checked) {
	console.log("Is checked now!");
    }
    else
	console.log("NOT checked now!");
 
    const formData = new FormData();
    formData.append('timsi', timsi);
    formData.append('channelid', channelid);
    formData.append('to', (el.checked==false) ? 'unsubscribed' : 'subscribed');
    el.disabled=true;
    const response = await fetch('change-subscription', { method: "POST", body: formData });
    if (response.ok === true) {
        const data = await response.json();
	console.log(data);
	el.checked = data["newstate"]=="subscribed";
	el.disabled=false;
	let s = document.getElementById(channelid);
	if(el.checked) 
	    s.innerText="Ingeschreven!";
	else
	    s.innerText="Afgemeld!";
    }
    else {
	console.log("error");
    }
    return false;
}


async function doPostUnsubscribe(userid, channelid, el)
{
    const response = await fetch(`unsubscribe/${userid}/${channelid}`, { method: "POST"});
    if (response.ok === true) {
	let s = document.getElementById("message");
	s.innerText="Unsubscribed!";
	el.disabled=true;
    }
    else {
	console.log("error");
    }
    return false;
}
